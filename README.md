# Mitsubishi AC WiFi Controller with MQTT support

Based on:
* https://github.com/jaisor/stus-rf24-wifi-gw
* https://github.com/SwiCago/HeatPump

# Programming the ESP8266

Compile and upload the project using USB. After the resistors are removed (see below), future updates will have to be made using OTA. 
Access to OTA is at `/update` path at the device's IP. In self-hosted AP mode, the device gives itself 192.168.4.1 IP address.

# Assembly

## Parts

## CN105 connector

## Removing the RX/TX resistors

## Wiring

## Case

# 

## Initial or boot after reset

After flash

## Resetting

The device will reset itself to default configuration and self-hosted AP if it unable to complete its boot sequence within 2 seconds (smooth boot) 3 times in a row.
This can be forced by power-cycling the device several times, each time powered up for about 1 second (less than 2 second, but enough for the CPU to start). 

