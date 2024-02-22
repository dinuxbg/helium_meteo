# Helium integration server for meteorological data

## Introduction

This project provides an integration HTTP server for accepting data from Helium meteo sensors.

This server is rather simple. Currently all it does is to record the POSTed temperature data in a local [SQLite](https://www.sqlite.org) database. Even then, it fulfills my own need to track data.

## Helium setup

From the Helium [console](https://console.helium.com/flows) do:
  1. Add your [device](https://console.helium.com/devices). Make sure you can see packets coming from the device.
  2. Add a new HTTP [integration](https://console.helium.com/integrations). Leave the default POST method option. Write the URL of your server.
  3. Go to the [Flows](https://console.helium.com/flows) menu and connect your device straight to your new integration. A decoder function is not needed.

With the above steps you should be able to see Helium POST requests to the URL you have provided.

## Server setup

Before you start the HTTP server on your premises, you need to initialize the database:

    $ ./init-db.py

Then you may run the server. It defaults to listening on port 8080:

    $ ./server.py

## Usage

There is no front-end yet to visualize the recorded data. For now you may run SQL queries to obtain meteorological logs. A few examples are provided:

    $ cat queries/dump-data.sql | sqlite3 meteo.db
