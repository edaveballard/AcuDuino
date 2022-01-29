import serial
import string
import threading
import sqlite3
import re
from datetime import datetime
import requests



#time to wait between updates in seconds
UPDATE_DELAY = 60
HOUR_BUFFER_LENGTH = 3600 // UPDATE_DELAY


weather_data = {}


def log_data():
    global weather_data
    db = sqlite3.connect('weather_history.db')
    db.execute("INSERT INTO weather_station_data VALUES (strftime('%s','now'),?,?,?,?,?,?,null)",(weather_data['tempf'],weather_data['windspeedmph'],weather_data['winddir'],weather_data['humidity'],weather_data['rainin'],weather_data['rainin_hr']))
    db.commit()
    db.close()

def send_data():
    global weather_data
    print("{} -- Temp: {}F Wind: {}MPH {} Humidity: {} Rainfall: {} (cntr {})".format(datetime.now(),float(weather_data['tempf']),float(weather_data['windspeedmph']),int(weather_data['winddir']),int(weather_data['humidity']),float(weather_data['rainin_hr']),float(weather_data['rainin']))) #https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php?ID=KMNWHITE51&PASSWORD=fb6MEZ8N&dateutc=now&humidity=57&windspeedmph=2&winddir=225&tempf=9&rainin=0&baromin=29.76&action=updateraw
    r = requests.get("https://weatherstation.wunderground.com/weatherstation/updateweatherstation.php?ID=KMNWHITE51&PASSWORD=fb6MEZ8N&dateutc=now&tempf={}&windspeedmph={}&winddir={}&windgustmph={}&humidity={}&rainin={}&action=updateraw".format(weather_data['tempf'],weather_data['windspeedmph'],weather_data['winddir'],weather_data['windgustmph'],weather_data['humidity'],weather_data['rainin_hr']))
    print("Received " + str(r.status_code) + " " + str(r.text) + " from WU")
    

def send_and_log():
    global weather_data
    threading.Timer(60.0, send_and_log).start()
    #presumes there is no rain in the last hour if just started
    if 'rainin_buffer' not in weather_data:
        weather_data['rainin_buffer'] = [weather_data['rainin'] for i in range(HOUR_BUFFER_LENGTH)]
    weather_data['rainin_hr'] = float(weather_data['rainin']) - float(weather_data['rainin_buffer'].pop(0))
    if weather_data['rainin_hr'] < 0:
        #rolls over every 100 inches
        weather_data['rainin_hr'] += 100.0
    weather_data['rainin_buffer'].append(weather_data['rainin'])
    #wind gust is highest recorded speed in the last hour
    if 'windspeedmph_buffer' not in weather_data:
        weather_data['windspeedmph_buffer'] = [0.0 for i in range(HOUR_BUFFER_LENGTH)]
    weather_data['windgustmph'] = max(weather_data['windspeedmph_buffer'])
    weather_data['windspeedmph_buffer'].pop(0)
    weather_data['windspeedmph_buffer'].append(float(weather_data['windspeedmph']))

    send_data()
    log_data()

#give ourselves 2 minutes to populate the data
threading.Timer(120.0, send_and_log).start()


ser = serial.Serial('/dev/ttyUSB0', 9600, 8, 'N', 1, timeout=16.0)


while True:
    output = ser.readline().decode('utf-8')
    if output != '':
        try:
            data = re.sub(r'[\r\n\s]+', '', output).split(",")
            for d in data:
                name, value = d.split(":")
                #TODO check to see if values have changed too much and rejett new values if so
                weather_data[name] = value
        except:
            print("Badly formed data, skipping")
