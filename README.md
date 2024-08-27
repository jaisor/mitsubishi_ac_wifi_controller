# Mitsubishi AC WiFi Controller with MQTT support

Based on:
* https://github.com/jaisor/stus-rf24-wifi-gw
* https://github.com/SwiCago/HeatPump

# Programming the ESP8266

Compile and upload the project using USB. After the resistors are removed (see below), future updates will have to be made using OTA. 
Access to OTA is at `/update` path at the device's IP. See Initial boot below on how to connect to the self-hosted AP.

# Assembly

## Parts

## CN105 connector

## Removing the RX/TX resistors

## Wiring

## Case

# 

## Initial boot / reset

On first boot the device creates a self-hosted WiFi access point (AP) with SSID starting with `ESP8266MHVAC` and WPA2 password `password123`
In self-hosted AP mode, the device gives itself `192.168.4.1` IP address.

The device can be connected to an existing AP using the `/wifi` option

![wifi screenshot](assets/ss1.png)

## Resetting

The device will reset itself to default configuration and self-hosted AP if it unable to complete its boot sequence within 2 seconds (smooth boot) 3 times in a row.
This can be forced by power-cycling the device several times, each time powered up for about 1 second (less than 2 second, but enough for the CPU to start). 

# Apache proxy setup

Example configuration for exposing a local network device via Apache proxy. Strong advised to use SSL/HTTPS and other appropriate authentication and authorization controls to prevent bad actors access. 

```
                <Location /ac>
                        AuthType Basic
                        Authname "Password Required"
                        AuthUserFile /etc/apache2/.htpasswd
                        Require valid-user
                </Location>

                ProxyPass /ac/roomname http://192.168.x.y disablereuse=On
                ProxyPassReverse /ac/roomname http://192.168.x.y
```