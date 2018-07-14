#include "bridge.hpp"
#include "plugin.hpp"
#include "rack.hpp"
#include "util/common.hpp"
#include "dsp/ringbuffer.hpp"

#include <unistd.h>
#if ARCH_WIN
	#include <winsock2.h>
#else
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <netinet/tcp.h>
	#include <fcntl.h>
#endif


#include <thread>
#include <utility>
#include <memory>

namespace rack {


struct BridgeClientConnection;
static BridgeClientConnection *connections[BRIDGE_NUM_PORTS] = {};
static AudioIO *audioListeners[BRIDGE_NUM_PORTS] = {};
static std::thread serverThread;
static bool serverRunning = false;
static BridgeMidiDriver *driver = NULL;


struct BridgeClientConnection {
	int client;
	bool ready = false;

	int port = -1;
	int sampleRate = 0;

	~BridgeClientConnection() {
		setPort(-1);
	}

	/** Returns true if successful */
	bool send(const void *buffer, int length) {
		if (length <= 0)
			return false;

#if ARCH_LIN
		int flags = MSG_NOSIGNAL;
#else
		int flags = 0;
#endif
		ssize_t remaining = 0;
		while (remaining < length) {
			ssize_t actual = ::send(client, (const char*) buffer, length, flags);
			if (actual <= 0) {
				ready = false;
				return false;
			}
			remaining += actual;
		}
		return true;
	}

	template <typename T>
	bool send(T x) {
		return send(&x, sizeof(x));
	}

	/** Returns true if successful */
	bool recv(void *buffer, int length) {
		if (length <= 0)
			return false;

#if ARCH_LIN
		int flags = MSG_NOSIGNAL;
#else
		int flags = 0;
#endif
		ssize_t remaining = 0;
		while (remaining < length) {
			ssize_t actual = ::recv(client, (char*) buffer + remaining, length - remaining, flags);
			if (actual <= 0) {
				ready = false;
				return false;
			}
			remaining += actual;
		}
		return true;
	}

	template <typename T>
	bool recv(T *x) {
		return recv(x, sizeof(*x));
	}

	void flush() {
		// Turn off Nagle
		int flag = 1;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*) &flag, sizeof(int));
		// Turn on Nagle
		flag = 0;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*) &flag, sizeof(int));
	}

	void run() {
		info("Bridge client connected");

		// Check hello key
		uint32_t hello = -1;
		recv<uint32_t>(&hello);
		if (hello != BRIDGE_HELLO) {
			info("Bridge client protocol mismatch %x %x", hello, BRIDGE_HELLO);
			return;
		}

		// Process commands until no longer ready
		ready = true;
		while (ready) {
			step();
		}

		info("Bridge client closed");
	}

	json_t* jsonCommand(json_t* inputJ) {
		json_t* outputJ = json_object();
		std::string error = "ok";
		json_t* commandJ = json_object_get(inputJ, "command");
		if (commandJ == NULL || !json_is_string(commandJ)) {
			error = "command node not found";
		} else {
			std::string command = json_string_value(commandJ);
			if (command == "listPlugins") {
				json_t *pluginsJ = json_object();
				json_object_set_new(outputJ, "plugins", pluginsJ);
				for (Plugin *plugin : gPlugins) {
					for (Model *model : plugin->models) {
						json_t *modelJ = json_object();
						json_t *tagsJ = json_array();
						std::string tags;
						for (ModelTag tag : model->tags) {
							if (tag != NO_TAG) {
								json_array_append(tagsJ, json_string(gTagNames[tag].c_str()));
							}
						}
						json_object_set_new(pluginsJ, model->slug.c_str(), modelJ);
						json_object_set_new(modelJ, "author", json_string(model->author.c_str()));
						json_object_set_new(modelJ, "name", json_string(model->name.c_str()));
						json_object_set_new(modelJ, "tags", tagsJ);
					}
				}
			} else if (command == "reset") {
				gRackWidget->runFunInGUI([] { gRackWidget->resetImpl(); });
			} else if (command == "createModule") {
				json_t* slugJ = json_object_get(inputJ, "slug");
				if (commandJ == NULL || !json_is_string(slugJ)) {
					error = "slug node not found";
				} else {
					std::string slug = json_string_value(slugJ);
					gRackWidget->runFunInGUI([slug, outputJ] { 
						ModuleWidget *moduleWidget = NULL;
						for (Plugin *plugin : gPlugins) {
								for (Model *model : plugin->models) {
									if (slug == model->slug) {
										moduleWidget = model->createModuleWidget();
										break;
									}
								}
						}
						if (moduleWidget) {
							gRackWidget->addModule(moduleWidget);
							moduleWidget->box.pos = Vec(0, 0);
							gRackWidget->requestModuleBoxNearest(moduleWidget, moduleWidget->box);
							
							// TODO: get top left absolute position on desktop of the module
							Vec topLeft(0, 61);
							float zoom = gRackScene->zoomWidget->zoom;
							
							Vec pos = moduleWidget->box.pos.mult(zoom).plus(topLeft);
							json_t *posJ = json_pack("[i, i]", (int) pos.x, (int) pos.y);
							json_object_set_new(outputJ, "pos", posJ);

							Vec size = moduleWidget->box.size.mult(zoom);
							json_t *sizeJ = json_pack("[i, i]", (int) size.x, (int) size.y);
							json_object_set_new(outputJ, "size", sizeJ);
						}
					});
				}
			} else {
				error = "invalid command";
			}
		}
		json_object_set_new(outputJ, "error", json_string(error.c_str()));
		return outputJ;
	}

	/** Accepts a command from the client */
	void step() {
		uint8_t command;
		if (!recv<uint8_t>(&command)) {
			return;
		}

		switch (command) {
			default:
			case NO_COMMAND: {
				warn("Bridge client: bad command %d detected, closing", command);
				ready = false;
			} break;

			case QUIT_COMMAND: {
				ready = false;
			} break;

			case PORT_SET_COMMAND: {
				uint8_t port = -1;
				recv<uint8_t>(&port);
				setPort(port);
			} break;

			case MIDI_MESSAGE_COMMAND: {
				MidiMessage message;
				if (!recv(&message, 3)) {
					return;
				}
				processMidi(message);
			} break;

			case AUDIO_SAMPLE_RATE_SET_COMMAND: {
				uint32_t sampleRate = 0;
				recv<uint32_t>(&sampleRate);
				setSampleRate(sampleRate);
			} break;

			case AUDIO_PROCESS_COMMAND: {
				uint32_t frames = 0;
				recv<uint32_t>(&frames);
				if (frames == 0 || frames > (1<<16)) {
					ready = false;
					return;
				}

				float input[BRIDGE_INPUTS * frames];
				if (!recv(&input, BRIDGE_INPUTS * frames * sizeof(float))) {
					debug("Failed to receive");
					return;
				}

				float output[BRIDGE_OUTPUTS * frames];
				memset(&output, 0, sizeof(output));
				processStream(input, output, frames);
				if (!send(&output, BRIDGE_OUTPUTS * frames * sizeof(float))) {
					debug("Failed to send");
					return;
				}
				// flush();
			} break;

			case JSON_COMMAND: {
				uint32_t inputLen = 0;
				if (!recv<uint32_t>(&inputLen)) {
					debug("Failed to receive");
					return;
				}
				printf("input length: %i\n", inputLen);
				std::unique_ptr<char[]> input(new char[inputLen + 1]);
				if (!recv(input.get(), inputLen * sizeof(char))) {
					debug("Failed to receive");
					return;
				}
				input[inputLen] = 0;
				printf("input: %s\n", input.get());
				json_error_t error;
				json_t* root = json_loads(input.get(), 0, &error);
				if (!root) {
					debug("json parsing error");
					return;
				}
				json_t* outputJ = jsonCommand(root);
				std::string output = json_dumps(outputJ, JSON_INDENT(2) | JSON_REAL_PRECISION(9));
				json_decref(root);
				json_decref(outputJ);
				uint32_t outputLen = output.size();
				if (!send(&outputLen, sizeof(uint32_t))) {
					debug("Failed to send");
					return;
				}
				if (!send(output.c_str(), outputLen)) {
					debug("Failed to send");
					return;
				}
			} break;
		}
	}

	void setPort(int port) {
		// Unbind from existing port
		if (0 <= this->port && connections[this->port] == this) {
			connections[this->port] = NULL;
		}

		// Bind to new port
		if ((0 <= port && port < BRIDGE_NUM_PORTS) && !connections[port]) {
			this->port = port;
			connections[this->port] = this;
			refreshAudio();
		}
		else {
			this->port = -1;
		}
	}

	void processMidi(MidiMessage message) {
		if (!(0 <= port && port < BRIDGE_NUM_PORTS))
			return;
		if (!driver)
			return;
		driver->devices[port].onMessage(message);
	}

	void setSampleRate(int sampleRate) {
		this->sampleRate = sampleRate;
		refreshAudio();
	}

	void processStream(const float *input, float *output, int frames) {
		if (!(0 <= port && port < BRIDGE_NUM_PORTS))
			return;
		if (!audioListeners[port])
			return;
		audioListeners[port]->setBlockSize(frames);
		audioListeners[port]->processStream(input, output, frames);
	}

	void refreshAudio() {
		if (!(0 <= port && port < BRIDGE_NUM_PORTS))
			return;
		if (connections[port] != this)
			return;
		if (!audioListeners[port])
			return;
		audioListeners[port]->setSampleRate(sampleRate);
	}
};


static void clientRun(int client) {
	defer({
#if ARCH_WIN
		if (shutdown(client, SD_SEND)) {
			warn("Bridge client shutdown() failed");
		}
		if (closesocket(client)) {
			warn("Bridge client closesocket() failed");
		}
#else
		if (close(client)) {
			warn("Bridge client close() failed");
		}
#endif
	});

#if ARCH_MAC
	// Avoid SIGPIPE
	int flag = 1;
	if (setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &flag, sizeof(int))) {
		warn("Bridge client setsockopt() failed");
		return;
	}
#endif

	// Disable non-blocking
#if ARCH_WIN
	unsigned long blockingMode = 0;
	if (ioctlsocket(client, FIONBIO, &blockingMode)) {
		warn("Bridge client ioctlsocket() failed");
		return;
	}
#else
	if (fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) & ~O_NONBLOCK)) {
		warn("Bridge client fcntl() failed");
		return;
	}
#endif

	BridgeClientConnection connection;
	connection.client = client;
	connection.run();
}


static void serverConnect() {
	// Initialize sockets
#if ARCH_WIN
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData)) {
		warn("Bridge server WSAStartup() failed");
		return;
	}
	defer({
		WSACleanup();
	});
#endif

	// Get address
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(BRIDGE_PORT);
#if ARCH_WIN
	addr.sin_addr.s_addr = inet_addr(BRIDGE_HOST);
#else
	inet_pton(AF_INET, BRIDGE_HOST, &addr.sin_addr);
#endif

	// Open socket
	int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0) {
		warn("Bridge server socket() failed");
		return;
	}
	defer({
		if (close(server)) {
			warn("Bridge server close() failed");
			return;
		}
		info("Bridge server closed");
	});

#if ARCH_MAC || ARCH_LIN
	int reuseAddrFlag = 1;
	setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuseAddrFlag, sizeof(reuseAddrFlag));
#endif

	// Bind socket to address
	if (bind(server, (struct sockaddr*) &addr, sizeof(addr))) {
		warn("Bridge server bind() failed");
		return;
	}

	// Listen for clients
	if (listen(server, 20)) {
		warn("Bridge server listen() failed");
		return;
	}
	info("Bridge server started");

	// Enable non-blocking
#if ARCH_WIN
	unsigned long blockingMode = 1;
	if (ioctlsocket(server, FIONBIO, &blockingMode)) {
		warn("Bridge server ioctlsocket() failed");
		return;
	}
#else
	int flags = fcntl(server, F_GETFL, 0);
	fcntl(server, F_SETFL, flags | O_NONBLOCK);
#endif

	// Accept clients
	while (serverRunning) {
		int client = accept(server, NULL, NULL);
		if (client < 0) {
			// Wait a bit before attempting to accept another client
			std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
			continue;
		}

		// Launch client thread
		std::thread clientThread(clientRun, client);
		clientThread.detach();
	}
}

static void serverRun() {
	while (serverRunning) {
		std::this_thread::sleep_for(std::chrono::duration<double>(0.1));
		serverConnect();
	}
}


std::vector<int> BridgeMidiDriver::getInputDeviceIds() {
	std::vector<int> deviceIds;
	for (int i = 0; i < 16; i++) {
		deviceIds.push_back(i);
	}
	return deviceIds;
}

std::string BridgeMidiDriver::getInputDeviceName(int deviceId) {
	if (deviceId < 0)
		return "";
	return stringf("Port %d", deviceId + 1);
}

MidiInputDevice *BridgeMidiDriver::subscribeInputDevice(int deviceId, MidiInput *midiInput) {
	if (!(0 <= deviceId && deviceId < 16))
		return NULL;

	devices[deviceId].subscribe(midiInput);
	return &devices[deviceId];
}

void BridgeMidiDriver::unsubscribeInputDevice(int deviceId, MidiInput *midiInput) {
	if (!(0 <= deviceId && deviceId < 16))
		return;

	devices[deviceId].unsubscribe(midiInput);
}


void bridgeInit() {
	serverRunning = true;
	serverThread = std::thread(serverRun);

	driver = new BridgeMidiDriver();
	midiDriverAdd(BRIDGE_DRIVER, driver);
}

void bridgeDestroy() {
	serverRunning = false;
	serverThread.join();
}

void bridgeAudioSubscribe(int port, AudioIO *audio) {
	if (!(0 <= port && port < BRIDGE_NUM_PORTS))
		return;
	// Check if an Audio is already subscribed on the port
	if (audioListeners[port])
		return;
	audioListeners[port] = audio;
	if (connections[port])
		connections[port]->refreshAudio();
}

void bridgeAudioUnsubscribe(int port, AudioIO *audio) {
	if (!(0 <= port && port < BRIDGE_NUM_PORTS))
		return;
	if (audioListeners[port] != audio)
		return;
	audioListeners[port] = NULL;
}


} // namespace rack
