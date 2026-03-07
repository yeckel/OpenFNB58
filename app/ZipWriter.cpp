#include "ZipWriter.h"
#include <QFile>

// ── CRC-32 (polynomial 0xEDB88320) ───────────────────────────────────────
quint32 ZipWriter::computeCrc32(const QByteArray& data)
{
    static const quint32 table[256] = {
        #define R(n) ((n)>>1^(((n)&1)?0xEDB88320u:0))
        #define R4(n) R(R(R(R(n))))
        #define R8(n) R4(R4(n))
        R8(0),R8(1),R8(2),R8(3),R8(4),R8(5),R8(6),R8(7),
        R8(8),R8(9),R8(10),R8(11),R8(12),R8(13),R8(14),R8(15),
        R8(16),R8(17),R8(18),R8(19),R8(20),R8(21),R8(22),R8(23),
        R8(24),R8(25),R8(26),R8(27),R8(28),R8(29),R8(30),R8(31),
        R8(32),R8(33),R8(34),R8(35),R8(36),R8(37),R8(38),R8(39),
        R8(40),R8(41),R8(42),R8(43),R8(44),R8(45),R8(46),R8(47),
        R8(48),R8(49),R8(50),R8(51),R8(52),R8(53),R8(54),R8(55),
        R8(56),R8(57),R8(58),R8(59),R8(60),R8(61),R8(62),R8(63),
        R8(64),R8(65),R8(66),R8(67),R8(68),R8(69),R8(70),R8(71),
        R8(72),R8(73),R8(74),R8(75),R8(76),R8(77),R8(78),R8(79),
        R8(80),R8(81),R8(82),R8(83),R8(84),R8(85),R8(86),R8(87),
        R8(88),R8(89),R8(90),R8(91),R8(92),R8(93),R8(94),R8(95),
        R8(96),R8(97),R8(98),R8(99),R8(100),R8(101),R8(102),R8(103),
        R8(104),R8(105),R8(106),R8(107),R8(108),R8(109),R8(110),R8(111),
        R8(112),R8(113),R8(114),R8(115),R8(116),R8(117),R8(118),R8(119),
        R8(120),R8(121),R8(122),R8(123),R8(124),R8(125),R8(126),R8(127),
        R8(128),R8(129),R8(130),R8(131),R8(132),R8(133),R8(134),R8(135),
        R8(136),R8(137),R8(138),R8(139),R8(140),R8(141),R8(142),R8(143),
        R8(144),R8(145),R8(146),R8(147),R8(148),R8(149),R8(150),R8(151),
        R8(152),R8(153),R8(154),R8(155),R8(156),R8(157),R8(158),R8(159),
        R8(160),R8(161),R8(162),R8(163),R8(164),R8(165),R8(166),R8(167),
        R8(168),R8(169),R8(170),R8(171),R8(172),R8(173),R8(174),R8(175),
        R8(176),R8(177),R8(178),R8(179),R8(180),R8(181),R8(182),R8(183),
        R8(184),R8(185),R8(186),R8(187),R8(188),R8(189),R8(190),R8(191),
        R8(192),R8(193),R8(194),R8(195),R8(196),R8(197),R8(198),R8(199),
        R8(200),R8(201),R8(202),R8(203),R8(204),R8(205),R8(206),R8(207),
        R8(208),R8(209),R8(210),R8(211),R8(212),R8(213),R8(214),R8(215),
        R8(216),R8(217),R8(218),R8(219),R8(220),R8(221),R8(222),R8(223),
        R8(224),R8(225),R8(226),R8(227),R8(228),R8(229),R8(230),R8(231),
        R8(232),R8(233),R8(234),R8(235),R8(236),R8(237),R8(238),R8(239),
        R8(240),R8(241),R8(242),R8(243),R8(244),R8(245),R8(246),R8(247),
        R8(248),R8(249),R8(250),R8(251),R8(252),R8(253),R8(254),R8(255)
    };
    quint32 crc = 0xFFFFFFFF;
    for (char c : data)
        crc = table[(crc ^ static_cast<quint8>(c)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFF;
}

void ZipWriter::writeLE16(QByteArray& buf, quint16 v)
{
    buf.append(static_cast<char>(v & 0xFF));
    buf.append(static_cast<char>((v >> 8) & 0xFF));
}
void ZipWriter::writeLE32(QByteArray& buf, quint32 v)
{
    buf.append(static_cast<char>(v & 0xFF));
    buf.append(static_cast<char>((v >> 8)  & 0xFF));
    buf.append(static_cast<char>((v >> 16) & 0xFF));
    buf.append(static_cast<char>((v >> 24) & 0xFF));
}

// ── API ───────────────────────────────────────────────────────────────────
void ZipWriter::addFile(const QString& name, const QByteArray& data)
{
    Entry e;
    e.name  = name;
    e.data  = data;
    e.crc32 = computeCrc32(data);
    m_entries.append(e);
}

bool ZipWriter::save(const QString& path)
{
    QByteArray archive;

    // ── Local file headers + data ─────────────────────────────────────────
    for (auto& e : m_entries) {
        e.offset = static_cast<quint32>(archive.size());

        QByteArray nameBuf = e.name.toUtf8();
        quint32 sz = static_cast<quint32>(e.data.size());

        // Local file header  (signature 0x04034b50)
        writeLE32(archive, 0x04034b50);
        writeLE16(archive, 20);    // version needed
        writeLE16(archive, 0);     // flags
        writeLE16(archive, 0);     // compression: STORE
        writeLE16(archive, 0);     // mod time
        writeLE16(archive, 0);     // mod date
        writeLE32(archive, e.crc32);
        writeLE32(archive, sz);    // compressed size (== uncompressed for STORE)
        writeLE32(archive, sz);    // uncompressed size
        writeLE16(archive, static_cast<quint16>(nameBuf.size()));
        writeLE16(archive, 0);     // extra field length
        archive.append(nameBuf);
        archive.append(e.data);
    }

    // ── Central directory ─────────────────────────────────────────────────
    quint32 cdOffset = static_cast<quint32>(archive.size());

    for (const auto& e : m_entries) {
        QByteArray nameBuf = e.name.toUtf8();
        quint32 sz = static_cast<quint32>(e.data.size());

        writeLE32(archive, 0x02014b50); // central dir signature
        writeLE16(archive, 0x0314);     // version made by (2.0, Unix)
        writeLE16(archive, 20);         // version needed
        writeLE16(archive, 0);          // flags
        writeLE16(archive, 0);          // compression: STORE
        writeLE16(archive, 0);          // mod time
        writeLE16(archive, 0);          // mod date
        writeLE32(archive, e.crc32);
        writeLE32(archive, sz);
        writeLE32(archive, sz);
        writeLE16(archive, static_cast<quint16>(nameBuf.size()));
        writeLE16(archive, 0);          // extra length
        writeLE16(archive, 0);          // file comment length
        writeLE16(archive, 0);          // disk start
        writeLE16(archive, 0);          // internal attrs
        writeLE32(archive, 0);          // external attrs
        writeLE32(archive, e.offset);   // local header offset
        archive.append(nameBuf);
    }

    quint32 cdSize = static_cast<quint32>(archive.size()) - cdOffset;

    // ── End of central directory ──────────────────────────────────────────
    quint16 n = static_cast<quint16>(m_entries.size());
    writeLE32(archive, 0x06054b50);
    writeLE16(archive, 0);      // disk number
    writeLE16(archive, 0);      // start disk
    writeLE16(archive, n);      // entries on this disk
    writeLE16(archive, n);      // total entries
    writeLE32(archive, cdSize);
    writeLE32(archive, cdOffset);
    writeLE16(archive, 0);      // comment length

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(archive);
    return true;
}
