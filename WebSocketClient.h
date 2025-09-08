#pragma once
#include <QObject>
#include <QTimer>
#include <QtWebSockets/QWebSocket>
#include <QJsonObject>
#include <QStringList>

class WebSocketClient : public QObject {
    Q_OBJECT
public:
    explicit WebSocketClient(QObject* parent = nullptr);

    void connectPublic();
    void subscribe(const QStringList& channels);
    int  call(const QString& method, const QJsonObject& params);

signals:
    void connected();
    void msgReceived(const QJsonObject& obj);
    void rpcReceived(int id, const QJsonObject& reply);

    // ---------- [After 後続は既存の slots] ----------
private slots:
    void onConnected();
    void onTextMessageReceived(const QString& msg);
    void onPing();

private:
    void sendJson(const QJsonObject& obj);

    QWebSocket m_ws;
    QTimer     m_pingTimer;
    bool       m_connected{ false };
    int        m_nextId{ 100 };
};
