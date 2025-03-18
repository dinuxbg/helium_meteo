#!/usr/bin/env python3

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Plot temperature data.

import sqlite3
import pandas
import matplotlib.pyplot as plt
import matplotlib.dates as pltdates
 

def plot(name):
    conn = sqlite3.connect("meteo.db")
 
    sql = """
SELECT datetime(reports.reported_at_ms / 1000, 'unixepoch', 'localtime') as t, measurements.temperature as temperature, measurements.pressure / 1000 as pressure, measurements.humidity as humidity
FROM (((((reports
INNER JOIN dev_eui ON dev_eui.id = reports.dev_eui_id)
INNER JOIN dev_addr ON dev_addr.id = reports.dev_addr_id)
INNER JOIN device_names ON device_names.id = reports.name_id)
INNER JOIN measurements ON measurements.report_id = reports.id)
INNER JOIN hotspot_connections ON hotspot_connections.report_id = reports.id)
WHERE device_names.id = (SELECT id FROM device_names WHERE device_names.name = ? )
GROUP BY reports.id;
"""
    data = pandas.read_sql(sql=sql, con=conn, params=(name, ))
 
    t = pltdates.date2num(data.t)

    plt.plot(t,data.temperature, label = "Temperature")
    plt.plot(t,data.pressure, label = "Pressure")
    plt.plot(t,data.humidity, label = "Humidity")
    plt.gca().xaxis.axis_date()
    plt.legend()
    plt.title("Meteo measurements")
    plt.show()

if __name__ == '__main__':
    from sys import argv

    if len(argv) != 2:
        print("Usage: ./plot.py name")
        exit(1)
    else:
        plot(argv[1])
