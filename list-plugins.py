#!/usr/bin/python3

# for the screenshot it needs pyautogui, see here how to install it: http://pyautogui.readthedocs.io/en/latest/install.html
# the other imports: "pip3 install numpy imutils opencv-python"

import os
import sys
import socket
import json
import struct
import numpy as np
import pyautogui
import imutils
import cv2
import time
import binascii

JSON_COMMAND = 6

tcpsocket = socket.socket(socket.AF_INET, socket.SOCK_STREAM) 

def sendUint8(num):
	tcpsocket.send(struct.pack('B', num))

def sendUint32(num):
	tcpsocket.send(struct.pack('I', num))

def readUint32():
	buf = b''
	while len(buf) < 4:
		buf += tcpsocket.recv(1)
	return struct.unpack('I', buf[:4])[0]

def sendString(string):
	sendUint32(len(string))
	tcpsocket.send(string.encode())
	
def readString():
	outputLen = readUint32()
	result = b''
	while len(result) < outputLen:
		result += tcpsocket.recv(1)
	return result.decode()
	
def sendJsonCommand(command):
	sendUint8(JSON_COMMAND)
	sendString(json.dumps(command))
	return json.loads(readString())

def saveScreenshot(slug, filename):
	sendJsonCommand({ 'command': 'reset' })
	module = sendJsonCommand({ 'command': 'createModule', 'slug': slug })
	time.sleep(0.5)  # wait a bit until the drawing is done and the module shows something interesting
	x = module['pos'][0]
	y = module['pos'][1]
	w = module['size'][0]
	h = module['size'][1]
	if w > 0 and h > 0:
		image = pyautogui.screenshot(region=(x, y, w, h))
		image = cv2.cvtColor(np.array(image), cv2.COLOR_RGB2BGR)
		cv2.imwrite(filename, image)
		return True
	else:
		print("no image, slug: " + slug)
		return False

# connect to VCV Rack and send BRIDGE_HELLO
tcpsocket.connect( ('127.0.0.1', 12512) ) 
sendUint32(0xff00fefd)

# get a list of all plugins
result = sendJsonCommand({ 'command': 'listPlugins' })

# create webpage with a screenshot of all plugins, sorted by author
crashList = [ ]
dir = 'plugin-list'
os.makedirs(dir, exist_ok=True)
web = "<html><head><title>VCV Rack Plugins</title></head><body><h1>VCV Rack Plugins</h1>\n"
authors = set()

for slug in result['plugins']:
	plugin = result['plugins'][slug]
	authors.add(plugin['author'])
authors = sorted(list(authors), key=lambda s: s.lower())
for author in authors:
	print(author)
	web += "<h2>" + author + "</h2>\n"
	plugins = []
	web += "<p>"
	for slug in result['plugins']:
		plugin = result['plugins'][slug]
		if author == plugin['author']:
			name = result['plugins'][slug]['name']
			plugins += [(slug, name)]
	plugins.sort(key=lambda tup: tup[1].lower())
	for (slug, name) in plugins:
		if not (slug in crashList):
			print("  " + name + " [" + slug + "]")
			web += "<b>" + name + "</b><br>\n"
			file = binascii.b2a_hex(slug.encode()).decode() + ".png"
			filename = dir + "/" + file
			save = False
			if not os.path.isfile(filename):
				if saveScreenshot(slug, filename):
					save = True
			else:
				save = True
			if save: web += '<img src="' + file + '"><br><br>\n'
	web += "</p>"
web += "</body></html>"
file = open(dir + '/index.html', 'w')
file.write(web)
file.close()
