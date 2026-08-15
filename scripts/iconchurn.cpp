// Tests whether repeatedly building and destroying a browser-sized list of icon
// pixmaps grows RSS - i.e. whether the per-open growth is Qt-side pixmap memory
// retained by the allocator rather than anything in MaterialSystem.
#include <QApplication>
#include <QListWidget>
#include <QIcon>
#include <QPixmap>
#include <QImage>
#include <malloc.h>
#include <cstdio>
#include <fstream>
#include <string>

static long residentKb()
{
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key)
        if (key == "VmRSS:") { long v = 0; status >> v; return v; }
    return 0;
}

int main(int argc, char** argv)
{
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    mallopt(M_TRIM_THRESHOLD, 4 * 1024 * 1024);
    QApplication app(argc, argv);
    const int items = argc > 1 ? std::atoi(argv[1]) : 8712;
    const int size = argc > 2 ? std::atoi(argv[2]) : 64;
    const bool trim = argc > 3 && std::string(argv[3]) == "trim";

    std::printf("baseline RSS = %ld KB (%d items @%dpx, trim=%d)\n",
                residentKb(), items, size, trim ? 1 : 0);
    for (int pass = 1; pass <= 4; ++pass) {
        {
            // One browser "open": a list of items each carrying a thumbnail.
            QListWidget list;
            for (int i = 0; i < items; ++i) {
                QImage image(size, size, QImage::Format_ARGB32);
                image.fill(static_cast<QRgb>(0xFF000000u | (i * 2654435761u)));
                auto* item = new QListWidgetItem(QIcon(QPixmap::fromImage(image)),
                                                 QStringLiteral("m%1").arg(i));
                list.addItem(item);
            }
            std::printf("  open  #%d peak = %ld KB\n", pass, residentKb());
            if (trim) { list.clear(); malloc_trim(0); }
        } // dialog closes here
        std::printf("  close #%d      = %ld KB\n", pass, residentKb());
    }
    return 0;
}
