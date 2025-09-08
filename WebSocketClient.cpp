#include "WebSocketClient.h"
#include <QJsonDocument>
#include <QJsonValue>
#include <QJsonObject>
#include <QJsonArray>

WebSocketClient::WebSocketClient(QObject* parent) : QObject(parent) {
    connect(&m_ws, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(&m_ws, &QWebSocket::textMessageReceived, this, &WebSocketClient::onTextMessageReceived);
    connect(&m_pingTimer, &QTimer::timeout, this, &WebSocketClient::onPing);
    m_pingTimer.setInterval(15000);
}

void WebSocketClient::connectPublic() {
    m_ws.open(QUrl(QStringLiteral("wss://www.deribit.com/ws/api/v2")));
}

void WebSocketClient::subscribe(const QStringList& channels) {
    QJsonObject params; params["channels"] = QJsonArray::fromStringList(channels);
    QJsonObject obj{ {"jsonrpc","2.0"},{"method","public/subscribe"},{"id",42},{"params",params} };
    sendJson(obj);
}

int WebSocketClient::call(const QString& method, const QJsonObject& params) {
    const int id = m_nextId++;
    QJsonObject obj{ {"jsonrpc","2.0"},{"method",method},{"id",id},{"params",params} };
    sendJson(obj);
    return id;
}

void WebSocketClient::onConnected() {
    // hello
    QJsonObject helloParams; helloParams["client_name"] = "BTC_OP_V2"; helloParams["client_version"] = "0.2";
    call("public/hello", helloParams);
    // heartbeat 30s
    QJsonObject hb; hb["interval"] = 30;
    call("public/set_heartbeat", hb);

    m_connected = true;
    m_pingTimer.start();
    emit connected();
}

void WebSocketClient::onTextMessageReceived(const QString& msg) {
    const auto doc = QJsonDocument::fromJson(msg.toUtf8());
    if (!doc.isObject()) return;
    const auto o = doc.object();

    // subscription か RPC応答かを振り分け
    if (o.contains("method") && o.value("method").toString() == "subscription") {
        emit msgReceived(o);
        return;
    }
    if (o.contains("id")) {
        emit rpcReceived(o.value("id").toInt(), o);
    }
}

void WebSocketClient::onPing() {
    QJsonObject nopParams;
    call("public/test", nopParams);
}

void WebSocketClient::sendJson(const QJsonObject& obj) {
    m_ws.sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}
