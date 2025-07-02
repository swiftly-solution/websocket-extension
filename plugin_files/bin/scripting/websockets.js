let websocketServerCB = {}
let websocketClientCB = {}

AddEventHandler("OnWSServerMessage", function (event, server_uuid, client_id, kind, message) {
    if (websocketServerCB.hasOwnProperty(server_uuid)) {
        websocketServerCB[server_uuid](server_uuid, client_id, kind, message)
    }
})

AddEventHandler("OnWSClientMessage", function (event, connection_uuid, kind, message) {
    if (websocketClientCB.hasOwnProperty(connection_uuid)) {
        websocketClientCB[connection_uuid](connection_uuid, kind, message)
    }
})

globalThis.ws = {
    StartServer: function (port, cb) {
        const server_uuid = websocket.StartListen(port)
        websocketServerCB[server_uuid] = cb
        return server_uuid
    },

    StopServer: function (server_uuid) {
        websocket.StopListen(server_uuid)

        if (websocketServerCB.hasOwnProperty(server_uuid)) {
            delete websocketServerCB[server_uuid];
        }
    },

    SendServerMessageToClient: function (server_uuid, client_id, message) {
        if (!message) message = "";
        if (typeof message != "string") message = (typeof message == "object" ? JSON.stringify(message) : String(message))

        websocket.SendServerMessageToClient(server_uuid, client_id, message)
    },

    SendServerMessageToAllClients: function (server_uuid, message) {
        if (!message) message = "";
        if (typeof message != "string") message = (typeof message == "object" ? JSON.stringify(message) : String(message))

        websocket.SendServerMessageToAllClients(server_uuid, message)
    },

    TerminateClientConnectionOnServer: function (server_uuid, client_id) {
        websocket.TerminateClientConnectionOnServer(server_uuid, client_id)
    },

    ConnectToServer: function (dns, port, isSecure, path, callback) {
        var connection_uuid = websocket.ConnectToServer(dns, port, isSecure, path)
        websocketClientCB[connection_uuid] = callback
        return connection_uuid
    },

    StopConnectionToServer: function (connection_uuid) {
        websocket.DisconnectFromServer(connection_uuid)

        if (websocketClientCB.hasOwnProperty(connection_uuid)) {
            delete websocketClientCB[connection_uuid];
        }
    },

    SendMessageToServer: function (connection_uuid, message) {
        if (!message) message = "";
        if (typeof message != "string") message = (typeof message == "object" ? JSON.stringify(message) : String(message))

        websocket.SendMessageToServer(connection_uuid, message)
    }
}
