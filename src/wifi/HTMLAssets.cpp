#include "wifi/HTMLAssets.h"

const String htmlTop = FPSTR("<html>\
  <head>\
  <meta charset='UTF-8'>\
  <title>%s</title>\
  <style>\
    body { background-color: #303030; font-family: 'Anaheim',sans-serif; Color: #d8d8d8; }\
    input, select { margin-bottom: 0.4em; }\
  </style>\
  </head>\
  <body>\
  <h1>%s - Mitsubishi AC Controller</h1>");

const String htmlBottom = FPSTR("<p><b>%s</b><br>\
  Uptime: <b>%02d:%02d:%02d</b><br/>\
  WiFi signal strength: <b>%i%%</b><br/>\
  Temperature: <b>%0.2f %s</b><br/>\
  Battery: <b>%0.2fV</b><br/>\
  MQTT: <b>%s</b>\
  </p></body>\
</html>");

const String htmlWifiApConnectForm = FPSTR("<hr><h2>Connect to WiFi Access Point (AP)</h2>\
  <form method='POST' action='/connect' enctype='application/x-www-form-urlencoded'>\
    <label for='ssid'>SSID (AP Name):</label><br>\
    <input type='text' id='ssid' name='ssid'><br><br>\
    <label for='pass'>Password (WPA2):</label><br>\
    <input type='password' id='pass' name='password' minlength='8' autocomplete='off' required><br><br>\
    <input type='submit' value='Connect...'>\
  </form>");

const String htmlDeviceConfigs = FPSTR("<hr><h2>Configs</h2>\
  <form method='POST' action='/config' enctype='application/x-www-form-urlencoded'>\
    <label for='deviceName'>Device name:</label><br>\
    <input type='text' id='deviceName' name='deviceName' value='%s'><br>\
    <label for='tempUnit'>Temperature units:</label><br>\
    <select name='tempUnit' id='tempUnit'>\
    %s\
    </select><br>\
    <br>\
    <label for='mqttServer'>MQTT server:</label><br>\
    <input type='text' id='mqttServer' name='mqttServer' value='%s'><br>\
    <label for='mqttPort'>MQTT port:</label><br>\
    <input type='text' id='mqttPort' name='mqttPort' value='%u'><br>\
    <label for='mqttTopic'>MQTT topic:</label><br>\
    <input type='text' id='mqttTopic' name='mqttTopic' value='%s'><br>\
    <br>\
    <label for='battVoltsDivider'>Battery volt measurement divider:</label><br>\
    <input type='text' id='battVoltsDivider' name='battVoltsDivider' value='%.2f'><br>\
    <br>\
    <input type='submit' value='Set...'>\
  </form>");

const String htmlHeatPump = FPSTR("<hr><h2>Heat Pump / AC Settings %s</h2>\
  %s\
  <div>Room temperature: <b>%0.1f %s</b></div><br>\
  <form method='POST' action='/hp' enctype='application/x-www-form-urlencoded'>\
    <label for='power'>Power:</label> \
    <select name='power' id='power'>\
    %s\
    </select><br>\
    <label for='mode'>Mode:</label> \
    <select name='mode' id='mode'>\
    %s\
    </select><br>\
    <label for='temperature'>Desired temperature:</label> \
    <input type='text' id='temperature' name='temperature' size='4' value='%0.1f'>%s<br>\
    <label for='fan'>Fan:</label> \
    <select name='fan' id='fan'>\
    %s\
    </select><br>\
    <label for='vane'>Vertical vane:</label> \
    <select name='vane' id='vane'>\
    %s\
    </select><br>\
    <label for='wideVane'>Horizontal vane:</label> \
    <select name='wideVane' id='wideVane'>\
    %s\
    </select><br>\
    <br>\
    <pre style='font-family: monospace; font-size: 9px;'>%s</pre>\
    <input type='submit' value='Set...'>\
  </form>");