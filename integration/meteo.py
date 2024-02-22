#!/usr/bin/env python3

# SPDX-License-Identifier: GPL-3.0-or-later
#
# Handle JSON data from Helium integration POST
# requests, parse, and record into an SQL database.
#
# sudo apt install python3-pycryptodome

# Assumptions:
#   - SQL INT can store entire EUI (64-bits).
import sys
import sqlite3
import base64
import json
import struct
import binascii

from Cryptodome.Cipher import AES

# Decoded payload from the device.
class Payload():
    def __init__(self):
        self.temperature = 0.0
        self.pressure_Pa = 0.0
        self.humidity_RH = 0.0
        self.battery_voltage = 0.0

    def decode(self, base64_str):
        payload_bin = base64.b64decode(base64_str)
        if len(payload_bin) == (AES.block_size + AES.key_size[0]):
            payload_bin = self.decrypt(payload_bin)
        payload_raw = struct.unpack('<iibh', payload_bin)
        self.temperature = payload_raw[0] / 1000.0 - 273.15
        self.pressure_Pa = payload_raw[1]
        self.humidity_RH = payload_raw[2]
        self.battery_voltage = payload_raw[3] / 1000.0

    def decrypt(self, enc):
        # To create a key use either one of the following commands: 
        #  $ dd if=/dev/random bs=16 count=1 | xxd -p > payload_aes_key.hex
        #  $ openssl rand -hex 16 > payload_aes_key.hex
        with open('payload_aes_key.hex') as f:
            key = binascii.unhexlify(f.readline().strip())
        iv = enc[:AES.block_size]
        cipher = AES.new(key, AES.MODE_CBC, iv)
        return cipher.decrypt(enc[AES.block_size:])

# Main class for parsing and handling JSON data from the Helium integration
# POST request.
#
# Reference: https://docs.helium.com/use-the-network/console/integrations/json-schema/
class Meteo():
    def __init__(self):
        self.conn = sqlite3.connect("meteo.db")

    # Generic method to acquire an ID from a given
    # strings table.  If the name does not exist,
    # it will create the necessary row, so that
    # a valid ID is always returned.
    def get_id_from_string(self, table, name_str):
        cur = self.conn.cursor()
        sel = 'SELECT id FROM ' + table + ' WHERE name = ?'
        cur.execute(sel, (name_str,))
        result = cur.fetchone()
        if result is not None:
            return result[0]
        else:
            sql = 'INSERT INTO ' + table + ' (name) VALUES (?)'
            vals = (name_str,)
            cur.execute(sql, vals)
            self.conn.commit()
            return cur.lastrowid

    def get_hotspot_id(self, name_str, lat, lng):
        cur = self.conn.cursor()
        sel = 'SELECT id FROM hotspot_names WHERE name = ?'
        cur.execute(sel, (name_str,))
        result = cur.fetchone()
        if result is not None:
            return result[0]
        else:
            sql = 'INSERT INTO hotspot_names (name, lat, lng) VALUES (?, ?, ?)'
            vals = (name_str, lat, lng)
            cur.execute(sql, vals)
            self.conn.commit()
            return cur.lastrowid

    # Insert an entry into the hotspot connections table.
    def record_hotspot(self, report_id, rec):
        sql = 'INSERT INTO hotspot_connections (report_id, frequency, name_id, rssi, snr) VALUES (?, ?, ?, ?, ?)'
        vals = (report_id,
                float(rec['frequency']),
                self.get_hotspot_id(rec['name'], float(rec['lat']), float(rec['long'])),
                float(rec['rssi']),
                float(rec['snr']))
        cur = self.conn.cursor()
        cur.execute(sql, vals)
        self.conn.commit()

    # Insert the device label (name), as described in the Helium integration
    # and passed in the Json metadata.
    def record_label(self, report_id, j):
        sql = 'INSERT INTO label_reports (report_id, name_id) VALUES (?, ?)'
        vals = (report_id,
                self.get_id_from_string('label_strings', j['name']))
        cur = self.conn.cursor()
        cur.execute(sql, vals)
        self.conn.commit()

    # Insert a new report entry.
    def record_report(self, rec, battery_voltage):
        sql = 'INSERT INTO reports (app_eui_id, dev_eui_id, dev_addr_id, dc_balance, fcnt, port, name_id, battery_voltage, reported_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)'
        vals = (self.get_id_from_string('app_eui', rec['app_eui']),
                self.get_id_from_string('dev_eui', rec['dev_eui']),
                self.get_id_from_string('dev_addr', rec['devaddr']),
                int(rec['dc']['balance']), 
                int(rec['fcnt']), 
                int(rec['port']), 
                self.get_id_from_string('device_names', rec['name']),
                battery_voltage,
                int(rec['reported_at']))
        cur = self.conn.cursor()
        cur.execute(sql, vals)
        self.conn.commit()
        report_id =  cur.lastrowid

        return report_id

    # Insert a new meteo measurement.
    def record_measurement(self, report_id, payload):
        sql = 'INSERT INTO measurements (report_id, temperature, pressure, humidity) VALUES (?, ?, ?, ?)'
        vals = (report_id,
                payload.temperature,
                payload.pressure_Pa,
                payload.humidity_RH)
        cur = self.conn.cursor()
        cur.execute(sql, vals)
        self.conn.commit()

    # Parse the given JSON string and then insert the
    # data into rows of the respective tables.
    def record(self, json_str):
        rec = json.loads(json_str)

        if rec['type'] == 'join':
            print('Received a JOIN packet.')
            return

        payload = Payload()
        payload.decode(rec['payload'])
        print(f'T={payload.temperature}°C, P={payload.pressure_Pa/100}hPa, RH={payload.humidity_RH}%, BAT={payload.battery_voltage}mV')

        report_id = self.record_report(rec, payload.battery_voltage)

        self.record_measurement(report_id, payload)
        for hotspot in rec['hotspots']:
            self.record_hotspot(report_id, hotspot)
        for label in rec['metadata']['labels']:
            self.record_label(report_id, label)

if __name__ == '__main__':
    t = Meteo()
    # If invoked from the command line, then
    # parse JSON lines from stdin. Useful
    # for quick integration testing.
    for line in sys.stdin:
        t.record(line)
