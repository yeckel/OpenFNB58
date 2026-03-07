#include "XlsxWriter.h"
#include <QXmlStreamWriter>
#include <cmath>

// ── Sheet management ──────────────────────────────────────────────────────
void XlsxWriter::addSheet(const QString& name)
{
    Sheet s;
    s.name = name;
    // Mark the second and later sheets as summary sheets
    s.isSummary = !m_sheets.isEmpty();
    m_sheets.append(s);
}

void XlsxWriter::setHeaders(const QStringList& headers)
{
    if (!m_sheets.isEmpty()) m_sheets.last().headers = headers;
}

void XlsxWriter::addRow(const QList<double>& values)
{
    if (!m_sheets.isEmpty()) m_sheets.last().rows.append(values);
}

void XlsxWriter::addSummaryRow(const QString& key, double value)
{
    if (!m_sheets.isEmpty())
        m_sheets.last().summaryRows.append({key, value});
}

// ── Cell reference helper ─────────────────────────────────────────────────
QString XlsxWriter::cellRef(int col, int row)
{
    // col is 1-based; convert to letter(s)
    QString colStr;
    int c = col;
    while (c > 0) {
        colStr.prepend(QChar('A' + (c - 1) % 26));
        c = (c - 1) / 26;
    }
    return colStr + QString::number(row);
}

// ── XML builders ─────────────────────────────────────────────────────────
QByteArray XlsxWriter::buildContentTypes(const QList<Sheet>& sheets)
{
    QByteArray buf;
    QXmlStreamWriter xml(&buf);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("Types");
    xml.writeDefaultNamespace("http://schemas.openxmlformats.org/package/2006/content-types");
    xml.writeEmptyElement("Default");
    xml.writeAttribute("Extension", "rels");
    xml.writeAttribute("ContentType",
        "application/vnd.openxmlformats-package.relationships+xml");
    xml.writeEmptyElement("Default");
    xml.writeAttribute("Extension", "xml");
    xml.writeAttribute("ContentType", "application/xml");
    xml.writeEmptyElement("Override");
    xml.writeAttribute("PartName", "/xl/workbook.xml");
    xml.writeAttribute("ContentType",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml");
    xml.writeEmptyElement("Override");
    xml.writeAttribute("PartName", "/xl/styles.xml");
    xml.writeAttribute("ContentType",
        "application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml");
    for (int i = 0; i < sheets.size(); ++i) {
        xml.writeEmptyElement("Override");
        xml.writeAttribute("PartName", QString("/xl/worksheets/sheet%1.xml").arg(i+1));
        xml.writeAttribute("ContentType",
            "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml");
    }
    xml.writeEndElement();
    xml.writeEndDocument();
    return buf;
}

QByteArray XlsxWriter::buildRels()
{
    QByteArray buf;
    QXmlStreamWriter xml(&buf);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("Relationships");
    xml.writeDefaultNamespace(
        "http://schemas.openxmlformats.org/package/2006/relationships");
    xml.writeEmptyElement("Relationship");
    xml.writeAttribute("Id", "rId1");
    xml.writeAttribute("Type",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument");
    xml.writeAttribute("Target", "xl/workbook.xml");
    xml.writeEndElement();
    xml.writeEndDocument();
    return buf;
}

QByteArray XlsxWriter::buildWorkbook(const QList<Sheet>& sheets)
{
    QByteArray buf;
    QXmlStreamWriter xml(&buf);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("workbook");
    xml.writeDefaultNamespace(
        "http://schemas.openxmlformats.org/spreadsheetml/2006/main");
    xml.writeNamespace(
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships", "r");
    xml.writeStartElement("sheets");
    for (int i = 0; i < sheets.size(); ++i) {
        xml.writeEmptyElement("sheet");
        xml.writeAttribute("name", sheets[i].name);
        xml.writeAttribute("sheetId", QString::number(i + 1));
        xml.writeAttribute("r:id", QString("rId%1").arg(i + 1));
    }
    xml.writeEndElement(); // sheets
    xml.writeEndElement(); // workbook
    xml.writeEndDocument();
    return buf;
}

QByteArray XlsxWriter::buildWorkbookRels(const QList<Sheet>& sheets)
{
    QByteArray buf;
    QXmlStreamWriter xml(&buf);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();
    xml.writeStartElement("Relationships");
    xml.writeDefaultNamespace(
        "http://schemas.openxmlformats.org/package/2006/relationships");
    for (int i = 0; i < sheets.size(); ++i) {
        xml.writeEmptyElement("Relationship");
        xml.writeAttribute("Id", QString("rId%1").arg(i + 1));
        xml.writeAttribute("Type",
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet");
        xml.writeAttribute("Target",
            QString("worksheets/sheet%1.xml").arg(i + 1));
    }
    xml.writeEmptyElement("Relationship");
    xml.writeAttribute("Id",   "rIdStyles");
    xml.writeAttribute("Type",
        "http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles");
    xml.writeAttribute("Target", "styles.xml");
    xml.writeEndElement();
    xml.writeEndDocument();
    return buf;
}

QByteArray XlsxWriter::buildStyles()
{
    // Minimal styles: default + bold (styleId=1)
    const char* xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <fonts count="2">
    <font><sz val="11"/><name val="Calibri"/></font>
    <font><b/><sz val="11"/><name val="Calibri"/><color rgb="FFFFFFFF"/></font>
  </fonts>
  <fills count="3">
    <fill><patternFill patternType="none"/></fill>
    <fill><patternFill patternType="gray125"/></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FF1F4E79"/></patternFill></fill>
  </fills>
  <borders count="1"><border><left/><right/><top/><bottom/><diagonal/></border></borders>
  <cellStyleXfs count="1"><xf numFmtId="0" fontId="0" fillId="0" borderId="0"/></cellStyleXfs>
  <cellXfs count="3">
    <xf numFmtId="0"  fontId="0" fillId="0" borderId="0" xfId="0"/>
    <xf numFmtId="0"  fontId="1" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1"/>
    <xf numFmtId="4"  fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1"/>
  </cellXfs>
</styleSheet>)";
    return QByteArray(xml);
}

QByteArray XlsxWriter::buildDataSheet(const Sheet& s)
{
    QByteArray buf;
    QXmlStreamWriter xml(&buf);
    xml.setAutoFormatting(false);
    xml.writeStartDocument();
    xml.writeStartElement("worksheet");
    xml.writeDefaultNamespace(
        "http://schemas.openxmlformats.org/spreadsheetml/2006/main");

    // Freeze top row
    xml.writeStartElement("sheetViews");
    xml.writeStartElement("sheetView");
    xml.writeAttribute("tabSelected", "1");
    xml.writeAttribute("workbookViewId", "0");
    xml.writeEmptyElement("pane");
    xml.writeAttribute("ySplit", "1");
    xml.writeAttribute("topLeftCell", "A2");
    xml.writeAttribute("activePane", "bottomLeft");
    xml.writeAttribute("state", "frozen");
    xml.writeEndElement(); // sheetView
    xml.writeEndElement(); // sheetViews

    xml.writeStartElement("sheetData");

    // Header row (style 1 = bold + blue background)
    if (!s.headers.isEmpty()) {
        xml.writeStartElement("row");
        xml.writeAttribute("r", "1");
        for (int c = 0; c < s.headers.size(); ++c) {
            xml.writeStartElement("c");
            xml.writeAttribute("r", cellRef(c + 1, 1));
            xml.writeAttribute("t", "inlineStr");
            xml.writeAttribute("s", "1");
            xml.writeStartElement("is");
            xml.writeTextElement("t", s.headers[c]);
            xml.writeEndElement(); // is
            xml.writeEndElement(); // c
        }
        xml.writeEndElement(); // row
    }

    // Data rows
    for (int ri = 0; ri < s.rows.size(); ++ri) {
        const auto& row = s.rows[ri];
        xml.writeStartElement("row");
        xml.writeAttribute("r", QString::number(ri + 2));
        for (int ci = 0; ci < row.size(); ++ci) {
            double v = row[ci];
            if (!std::isfinite(v)) continue; // skip NaN → empty cell
            xml.writeStartElement("c");
            xml.writeAttribute("r", cellRef(ci + 1, ri + 2));
            xml.writeAttribute("s", "2"); // number format
            xml.writeStartElement("v");
            xml.writeCharacters(QString::number(v, 'f', 6));
            xml.writeEndElement(); // v
            xml.writeEndElement(); // c
        }
        xml.writeEndElement(); // row
    }

    xml.writeEndElement(); // sheetData
    xml.writeEndElement(); // worksheet
    xml.writeEndDocument();
    return buf;
}

QByteArray XlsxWriter::buildSummarySheet(const Sheet& s)
{
    QByteArray buf;
    QXmlStreamWriter xml(&buf);
    xml.setAutoFormatting(false);
    xml.writeStartDocument();
    xml.writeStartElement("worksheet");
    xml.writeDefaultNamespace(
        "http://schemas.openxmlformats.org/spreadsheetml/2006/main");
    xml.writeStartElement("sheetData");

    int row = 1;
    for (const auto& [key, val] : s.summaryRows) {
        xml.writeStartElement("row");
        xml.writeAttribute("r", QString::number(row));
        // Key cell
        xml.writeStartElement("c");
        xml.writeAttribute("r", cellRef(1, row));
        xml.writeAttribute("t", "inlineStr");
        xml.writeStartElement("is");
        xml.writeTextElement("t", key);
        xml.writeEndElement();
        xml.writeEndElement();
        // Value cell
        xml.writeStartElement("c");
        xml.writeAttribute("r", cellRef(2, row));
        xml.writeAttribute("s", "2");
        xml.writeTextElement("v", QString::number(val, 'g', 8));
        xml.writeEndElement();
        xml.writeEndElement(); // row
        ++row;
    }

    xml.writeEndElement(); // sheetData
    xml.writeEndElement(); // worksheet
    xml.writeEndDocument();
    return buf;
}

// ── Save ──────────────────────────────────────────────────────────────────
bool XlsxWriter::save(const QString& path)
{
    if (m_sheets.isEmpty()) {
        m_error = "No sheets added";
        return false;
    }

    ZipWriter zip;
    zip.addFile("[Content_Types].xml", buildContentTypes(m_sheets));
    zip.addFile("_rels/.rels",         buildRels());
    zip.addFile("xl/workbook.xml",     buildWorkbook(m_sheets));
    zip.addFile("xl/_rels/workbook.xml.rels", buildWorkbookRels(m_sheets));
    zip.addFile("xl/styles.xml",       buildStyles());

    for (int i = 0; i < m_sheets.size(); ++i) {
        const auto& s = m_sheets[i];
        QByteArray sheet = s.isSummary ? buildSummarySheet(s) : buildDataSheet(s);
        zip.addFile(QString("xl/worksheets/sheet%1.xml").arg(i + 1), sheet);
    }

    if (!zip.save(path)) {
        m_error = "Cannot write file: " + path;
        return false;
    }
    return true;
}
