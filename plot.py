#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt

data = pd.read_csv('data.csv')

plt.plot(data['time'], data['value'])
plt.xticks(rotation=45)
plt.ylabel('Watt')
plt.show()