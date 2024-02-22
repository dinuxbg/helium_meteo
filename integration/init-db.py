#!/usr/bin/env python3

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Assumptions:
#   - SQL INT can store entire EUI (64-bits).
import sqlite3

def get_db_cursor():
    con = sqlite3.connect("meteo.db")
    cur = con.cursor()
    return cur


def main():
    cur = get_db_cursor()
    # Battery voltage is really part of the payload, and not part of the Integration
    # JSON record. But I consider this an implementation detail.
    # The logical place for battery voltage is in the overall report, rather
    # than the GPS point coordinates table.
    cur.execute('CREATE TABLE reports('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'app_eui_id INTEGER,'
                    'dev_eui_id INTEGER,'
                    'dev_addr_id INTEGER,'
                    'dc_balance INTEGER,'
                    'fcnt INTEGER,'
                    'port INTEGER,'
                    'name_id INTEGER,'
                    'battery_voltage REAL,'
                    'reported_at_ms UNSIGNED BIGINT,'
                    'FOREIGN KEY(app_eui_id) REFERENCES app_eui(id),'
                    'FOREIGN KEY(dev_eui_id) REFERENCES dev_eui(id),'
                    'FOREIGN KEY(dev_addr_id) REFERENCES dev_addr(id),'
                    'FOREIGN KEY(name_id) REFERENCES device_names(id))')
    cur.execute('CREATE TABLE hotspot_connections('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'report_id INTEGER NOT NULL,'
                    'frequency REAL,'
                    'name_id INTEGER,'
                    'rssi REAL,'
                    'snr REAL,'
                    'FOREIGN KEY(report_id) REFERENCES reports(id),'
                    'FOREIGN KEY(name_id) REFERENCES hotspot_names(id))')
    cur.execute('CREATE TABLE hotspot_names('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'name VARCHAR(128),'
                    'lat REAL,'
                    'lng REAL)')
    cur.execute('CREATE TABLE device_names('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'name VARCHAR(128))')
    cur.execute('CREATE TABLE measurements('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'report_id INTEGER NOT NULL,'
                    'temperature REAL,'
                    'pressure REAL,'
                    'humidity REAL,'
                    'FOREIGN KEY(report_id) REFERENCES reports(id))')
    cur.execute('CREATE TABLE label_reports('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'report_id INTEGER NOT NULL,'
                    'name_id INTEGER NOT NULL,'
                    'FOREIGN KEY(report_id) REFERENCES reports(id),'
                    'FOREIGN KEY(name_id) REFERENCES label_strings(id))')
    cur.execute('CREATE TABLE label_strings('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'name VARCHAR(128))')
    # Store EUI as strings because SQLite3 Python
    # binding cannot handle 64-bit Python integers,
    # even when declaring columns as UNSIGNED BIGINT.
    cur.execute('CREATE TABLE app_eui('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'name VARCHAR(16))')
    cur.execute('CREATE TABLE dev_eui('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'name VARCHAR(16))')
    cur.execute('CREATE TABLE dev_addr('
                    'id INTEGER PRIMARY KEY AUTOINCREMENT,'
                    'name VARCHAR(16))')

if __name__ == '__main__':
    main()
