#pragma once
#include "ZipWriter.h"
#include "DataRecord.h"
#include <QString>
#include <QStringList>
#include <QList>

// Produces a minimal .xlsx file (OOXML Strict) using ZipWriter.
// Supports multiple sheets; the first sheet gets the full data table.
class XlsxWriter
{
public:
    XlsxWriter() = default;

    void setTitle(const QString& title)    { m_title = title; }

    // Add a new sheet (call before adding rows to it).
    void addSheet(const QString& name);

    // Set column headers for the current sheet.
    void setHeaders(const QStringList& headers);

    // Add a data row to the current sheet (NaN values become empty cells).
    void addRow(const QList<double>& values);

    // Add a two-column key/value row to the current sheet (for summaries).
    void addSummaryRow(const QString& key, double value);

    // Write to file; returns true on success.
    bool save(const QString& path);

    QString lastError() const { return m_error; }

private:
    struct Sheet {
        QString name;
        QStringList headers;
        QList<QList<double>> rows;
        QList<QPair<QString,double>> summaryRows;
        bool isSummary = false;
    };

    QString        m_title;
    QList<Sheet>   m_sheets;
    QString        m_error;

    static QByteArray buildContentTypes(const QList<Sheet>& sheets);
    static QByteArray buildRels();
    static QByteArray buildWorkbook(const QList<Sheet>& sheets);
    static QByteArray buildWorkbookRels(const QList<Sheet>& sheets);
    static QByteArray buildStyles();
    static QByteArray buildDataSheet(const Sheet& s);
    static QByteArray buildSummarySheet(const Sheet& s);
    static QString cellRef(int col, int row); // e.g. col=1,row=1 → "A1"
};
