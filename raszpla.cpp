#include <dirent.h>
#include <format>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>

struct Domain
{
    FILE* hnd;
    uint64_t overflow;
    uint64_t value;
    std::string name;
};

struct Measurement
{
    uint64_t value;
    timespec time;
};

std::vector<Domain> g_domains;
std::vector<Measurement> g_data;

bool g_exit = false;

static void Probe( const char* path, int parent = -1 )
{
    DIR* dir = opendir( path );
    if( !dir ) return;

    struct dirent* ent;
    uint64_t maxRange = 0;
    uint64_t value = 0;
    std::string name;
    FILE* hnd = nullptr;

    while( ( ent = readdir( dir ) ) )
    {
        if( ent->d_type == DT_REG )
        {
            if( strcmp( ent->d_name, "max_energy_range_uj" ) == 0 )
            {
                const auto fn = std::format( "{}/max_energy_range_uj", path );
                FILE* f = fopen( fn.c_str(), "r" );
                if( f )
                {
                    fscanf( f, "%" PRIu64, &maxRange );
                    fclose( f );
                }
            }
            else if( strcmp( ent->d_name, "name" ) == 0 )
            {
                const auto fn = std::format( "{}/name", path );
                FILE* f = fopen( fn.c_str(), "r" );
                if( f )
                {
                    char ntmp[128];
                    if( fgets( ntmp, 128, f ) )
                    {
                        if( parent < 0 )
                        {
                            name.assign( ntmp, strlen( ntmp ) - 1 );
                        }
                        else
                        {
                            name = std::format( "{}/{}", g_domains[parent].name, ntmp );
                            name.pop_back();
                        }
                    }
                    fclose( f );
                }
            }
            else if( strcmp( ent->d_name, "energy_uj" ) == 0 )
            {
                const auto fn = std::format( "{}/energy_uj", path );
                hnd = fopen( fn.c_str(), "r" );
            }

            if( !name.empty() && hnd && maxRange > 0 ) break;
        }
    }

    if( !name.empty() && hnd && maxRange > 0 )
    {
        parent = g_domains.size();
        g_domains.emplace_back( Domain {
            .hnd = hnd,
            .overflow = maxRange,
            .value = value,
            .name = name
        } );
    }
    else if( hnd )
    {
        fclose( hnd );
    }

    rewinddir( dir );
    while( ( ent = readdir( dir ) ) )
    {
        if( ent->d_type == DT_DIR && strncmp( ent->d_name, "intel-rapl:", 11 ) == 0 )
        {
            const auto fn = std::format( "{}/{}", path, ent->d_name );
            Probe( fn.c_str(), parent );
        }
    }

    closedir( dir );
}

static void Collect( Domain& domain )
{
    char tmp[32];
    uint64_t value = domain.value;

    while( !g_exit )
    {
        if( fread( tmp, 1, 32, domain.hnd ) > 0 )
        {
            rewind( domain.hnd );
            auto p = (uint64_t)atoll( tmp );
            uint64_t delta;
            if( p >= value )
            {
                delta = p - value;
            }
            else
            {
                delta = domain.overflow - value + p;
            }
            value = p;

            timespec ts;
            clock_gettime( CLOCK_REALTIME, &ts );

            g_data.emplace_back( Measurement {
                .value = delta,
                .time = ts
            } );
        }
        usleep( 100 * 1000 );
    }
}

int main( int argc, char** argv )
{
    Probe( "/sys/devices/virtual/powercap/intel-rapl" );

    if( g_domains.empty() )
    {
        printf( "No RAPL domains found.\n" );
        if( getuid() != 0 )
        {
            printf( "You need to run this program as root.\n" );
        }
        return 1;
    }

    if( argc == 1 )
    {
        printf( "Usage: raszpla <domain>\n\n" );
        printf( "Available domains:\n" );
        int idx = 0;
        for( auto& domain : g_domains )
        {
            printf( "  %i: %s\n", idx++, domain.name.c_str() );
        }
        return 0;
    }

    int didx = atoi( argv[1] );
    if( didx < 0 || didx >= (int)g_domains.size() )
    {
        printf( "Invalid domain index.\n" );
        return 1;
    }

    int idx = 0;
    for( auto& domain : g_domains )
    {
        if( idx != didx ) fclose( domain.hnd );
    }

    printf( "Collecting data from domain %i: %s\n", didx, g_domains[didx].name.c_str() );
    printf( "Press Ctrl+C to stop.\n" );

    g_data.reserve( 1024 * 1024 );
    signal( SIGINT, []( int ) { g_exit = true; } );
    Collect( g_domains[didx] );
    signal( SIGINT, SIG_DFL );

    FILE* f = fopen( "data.csv", "w" );
    if( f )
    {
        char buf[32];
        fprintf( f, "time,value\n" );
        for( size_t i=1; i<g_data.size(); i++ )
        {
            const auto& m = g_data[i];
            const auto td = ( m.time.tv_sec - g_data[i-1].time.tv_sec ) * 1e9 + m.time.tv_nsec - g_data[i-1].time.tv_nsec;
            if( td == 0 ) continue;
            auto tm = localtime( &m.time.tv_sec );
            strftime( buf, 32, "%Y-%m-%d %H:%M:%S", tm );
            fprintf( f, "%s.%01lu,%f\n", buf, m.time.tv_nsec / 100000000, m.value * 1000. / td );
        }
        fclose( f );
    }
}
