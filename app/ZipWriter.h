#pragma once
#include <QString>
#include <QByteArray>
#include <QList>

// Minimal STORE-only ZIP writer (no compression needed for XLSX).
// Supports adding multiple files, then writing to a path.
class ZipWriter
{
public:
    ZipWriter() = default;

    // Add a named file with raw (uncompressed) data.
    void addFile(const QString& name, const QByteArray& data);

    // Write the ZIP archive to the given filesystem path.
    // Returns true on success.
    bool save(const QString& path);

private:
    struct Entry {
        QString    name;
        QByteArray data;
        quint32    crc32  = 0;
        quint32    offset = 0; // local header offset in the stream
    };
    QList<Entry> m_entries;

    static quint32 computeCrc32(const QByteArray& data);
    static void writeLE16(QByteArray& buf, quint16 v);
    static void writeLE32(QByteArray& buf, quint32 v);
};
