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
import datetime

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
    def record_hotspot(self, report_id, rec, frequency_hZ):
        sql = 'INSERT INTO hotspot_connections (report_id, frequency, name_id, rssi, snr) VALUES (?, ?, ?, ?, ?)'
        vals = (report_id,
                int(frequency_hZ),
                self.get_hotspot_id(rec['metadata']['gateway_name'], float(rec['metadata']['gateway_lat']), float(rec['metadata']['gateway_long'])),
                float(rec['rssi']),
                float(rec['snr'] if 'snr' in rec else -1000000))
        cur = self.conn.cursor()
        cur.execute(sql, vals)
        self.conn.commit()

    # Insert a new report entry.
    def record_report(self, rec, battery_voltage):
        sql = 'INSERT INTO reports (dev_eui_id, dev_addr_id, dc_balance, fcnt, port, name_id, profile_id, battery_voltage, reported_at_ms) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)'
        epoch_timestamp_ms = datetime.datetime.fromisoformat(rec['time']).timestamp() * 1000
        vals = (self.get_id_from_string('dev_eui', rec['deviceInfo']['devEui']),
                self.get_id_from_string('dev_addr', rec['devAddr']),
                int(rec['dc']['balance'] if 'dc' in rec else -1),
                int(rec['fCnt']),
                int(rec['fPort']),
                self.get_id_from_string('device_names', rec['deviceInfo']['deviceName']),
                self.get_id_from_string('profile_names', rec['deviceInfo']['deviceProfileName']),
                battery_voltage,
                int(epoch_timestamp_ms))
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

        payload = Payload()
        payload.decode(rec['data'])
        print(f'T={payload.temperature}°C, P={payload.pressure_Pa/100}hPa, RH={payload.humidity_RH}%, BAT={payload.battery_voltage}mV')

        report_id = self.record_report(rec, payload.battery_voltage)

        self.record_measurement(report_id, payload)
        for hotspot in rec['rxInfo']:
            self.record_hotspot(report_id, hotspot, float(rec['txInfo']['frequency']))

if __name__ == '__main__':
    t = Meteo()
    # If invoked from the command line, then
    # parse JSON lines from stdin. Useful
    # for quick integration testing.
    for line in sys.stdin:
        t.record(line)
