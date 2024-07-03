# Helium integration server for meteorological data

## Introduction

This project provides an integration HTTP server for accepting data from Helium meteo sensors.

This server is rather simple. Currently all it does is to record the POSTed temperature data in a local [SQLite](https://www.sqlite.org) database. Even then, it fulfills my own need to track data.

## Pick a LoRaWAN Network Server Provider
First you need to pick a LoRaWAN Network Server Provider from the [official list](https://docs.helium.com/iot/find-a-lns-provider/). It is needed to route the data packets sent by the sensor to your integration HTTP server.

Make an account in the LNS you picked. Some LNS offer a small number of free initial [DC](https://docs.helium.com/tokens/data-credit/), but in general you'd need to pay a small sum to buy a batch of DC.

## Helium setup
The following instructions are a summary of the [official manual](https://docs.helium.com/console/adding-devices/) from Helium.

Login in your provider and:
 * Create a new Device Profile.
 * Create a new Application.
   ** In the Application page, add a new Device.
   ** Generate a new `dev_eui` for the device by clicking on the spiral arrow button.
   ** Leave the `join_eui` field empty.
   ** Select the profile you previously created.
   ** Click Submit. You'll be prompted for Application key. Generate a new one, and click Submit.
 * In the Application page click on Integrations.
   ** Add HTTP integration.
   ** Fill-in the URL of your integration server (i.e. the python program in this directory).

With the above steps you should be able to see Helium POST requests to the URL you have provided.

Don't forget to configure your device with the `dev_eui` and `app_key` generated from the above steps.

## Server setup

Before you start the HTTP server on your premises, you need to initialize the database:

    $ ./init-db.py

Then you may run the server. It defaults to listening on port 8080:

    $ ./server.py

## Usage

There is no front-end yet to visualize the recorded data. For now you may run SQL queries to obtain meteorological logs. A few examples are provided:

    $ cat queries/dump-data.sql | sqlite3 meteo.db
