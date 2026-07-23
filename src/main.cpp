#include "main.h"
#include "network.h"
#include "task.h"

/* Global state — exposed via extern in main.h */
String authorizationToken = "";
WebSocketsClient webSocket;
QueueHandle_t commandQueue = NULL;
bool isWSConnected = false;

void setup(void)
{
#if DEBUG_ENABLE
	Serial.begin(115200);
#endif
	DEBUG_PRINTLN("Device starting...");

	if (!connectToWiFi()) {
		DEBUG_PRINTLN("WiFi connection failed, restarting device...");
		ESP.restart();
		return;
	}

	if (!authbydeviceid()) {
		DEBUG_PRINTLN("Authentication failed, restarting device...");
		ESP.restart();
		return;
	}

	commandQueue = xQueueCreate(10, sizeof(CommandMsg));
	if (commandQueue == NULL) {
		DEBUG_PRINTLN("Failed to create command queue");
		ESP.restart();
	}

	/* Fetch and enqueue the latest pending command before starting tasks. */
	syncPendingCommands();

	webSocket.beginSSL(WSS_HOST, 443, WSS_URL);
	webSocket.setExtraHeaders("Origin=https://" API_HOST);
	webSocket.setAuthorization(authorizationToken.c_str());
	webSocket.setReconnectInterval(5000);
	webSocket.onEvent(webSocketEvent);

	/* Core 0: periodic HTTP upload. */
	xTaskCreatePinnedToCore(httpUploadTask, "HttpUploadTask", 10240,
				NULL, 1, NULL, 0);

	/* Core 1: command processing, offloads JSON parsing from ISR. */
	xTaskCreatePinnedToCore(cmdProcessTask, "CmdProcessTask", 8192,
				NULL, 2, NULL, 1);

	DEBUG_PRINTLN("System init complete");
}

void loop(void)
{
	if (WiFi.status() != WL_CONNECTED) {
		DEBUG_PRINTLN("WiFi disconnected, attempting to reconnect...");
		isWSConnected = false;
		if (!connectToWiFi() || !authbydeviceid()) {
			DEBUG_PRINTLN("Reconnection failed, restarting...");
			ESP.restart();
			return;
		}
		webSocket.setAuthorization(authorizationToken.c_str());
	}

	webSocket.loop();

	/*
	 * If WebSocket is down while WiFi is still up the library
	 * auto-reconnects via setReconnectInterval().  Log the condition
	 * at most once per 10 s to avoid spam.
	 */
	if (!isWSConnected && WiFi.status() == WL_CONNECTED) {
		static unsigned long lastReconnectLog = 0;
		if (millis() - lastReconnectLog > 10000) {
			DEBUG_PRINTLN("WebSocket disconnected, waiting for auto-reconnect...");
			lastReconnectLog = millis();
		}
	}

	delay(50);
}

/**
 * webSocketEvent - WebSocket library callback.
 * @type:    event type (connected, disconnected, text, error, ping, pong).
 * @payload: raw frame data, valid only during this callback.
 * @length:  payload byte count.
 *
 * On WStype_TEXT enqueues a CommandMsg for asynchronous processing.
 * On WStype_DISCONNECTED resets the command queue so stale entries are
 * discarded; the next connect will re-sync via syncPendingCommands().
 */
void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
	switch (type) {
	case WStype_DISCONNECTED:
		DEBUG_PRINTLN("WebSocket disconnected");
		isWSConnected = false;
		xQueueReset(commandQueue);
		break;
	case WStype_CONNECTED:
		DEBUG_PRINTLN("WebSocket connected");
		isWSConnected = true;
		webSocket.sendPing(NULL, 0);
		break;
	case WStype_TEXT: {
		DEBUG_PRINTF("Message received: %s\n", (char *)payload);
		CommandMsg cmdMsg;
		cmdMsg.payload = (char *)payload;
		cmdMsg.length = length;
		if (xQueueSend(commandQueue, &cmdMsg, 0) != pdPASS)
			DEBUG_PRINTLN("Command queue full, dropping message");
		break;
	}
	case WStype_ERROR:
		DEBUG_PRINTF("WebSocket error: %s\n", (char *)payload);
		isWSConnected = false;
		break;
	case WStype_PING:
		DEBUG_PRINTLN("Ping received");
		break;
	case WStype_PONG:
		DEBUG_PRINTLN("Pong received");
		break;
	default:
		DEBUG_PRINTF("Unknown event type: %s\n", (char *)payload);
		break;
	}
}
