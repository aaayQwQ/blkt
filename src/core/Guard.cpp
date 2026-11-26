#include "Guard.hpp"

#include <QRegularExpression>

namespace blkt {

QString Guard::cleanText(const QString &value, qsizetype maxLength)
{
    QString result;
    result.reserve(qMin(value.size(), maxLength));

    for (const QChar ch : value) {
        if (result.size() >= maxLength)
            break;

        const ushort u = ch.unicode();
        if (u == 0x0000 || u == 0x007f || (u < 0x0020 && u != u'\n' && u != u'\t'))
            continue;
        result.append(ch);
    }

    return result.trimmed();
}

QString Guard::redact(const QString &value)
{
    QString result = value;
    static const QRegularExpression secretPattern(
        QStringLiteral(R"((cookie|set-cookie|csrf(?:_token)?|authorization|access[_-]?token|api[_-]?key|secret)\s*[:=]\s*[^\s,;]+)"),
        QRegularExpression::CaseInsensitiveOption);

    result.replace(secretPattern, QStringLiteral("\\1=[REDACTED]"));
    result.replace(QRegularExpression(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]")),
                   QString());
    if (result.size() > 2048)
        result.truncate(2048);
    return result;
}

bool Guard::validRoomId(const QString &roomId)
{
    if (roomId.isEmpty() || roomId.size() > 20)
        return false;

    bool ok = false;
    const qulonglong value = roomId.toULongLong(&ok, 10);
    return ok && value > 0;
}

bool Guard::validCookie(const QByteArray &cookie)
{
    if (cookie.isEmpty() || cookie.size() > 8192)
        return false;

    for (const char ch : cookie) {
        const uchar u = static_cast<uchar>(ch);
        if (u < 0x20 || u >= 0x7f)
            return false;
    }
    return true;
}

bool Guard::validCsrf(const QString &csrf)
{
    if (csrf.isEmpty() || csrf.size() > 128)
        return false;

    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]+$"));
    return pattern.match(csrf).hasMatch();
}

} // namespace blkt
