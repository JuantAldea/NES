#include <QApplication>
#include <QFile>

#include <QDebug>
#include <QFileDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QColor>
#include <QLineEdit>
#include <QCheckBox>
#include <QLabel>
#include <QTableWidget>
#include "external/QHexView/document/buffer/qmemorybuffer.h"
#include "external/QHexView/qhexview.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QWidget w;
    w.setLayout(new QHBoxLayout);

    QHexView hexview(&w);
    QGroupBox cpu_group(&w);
    w.layout()->addWidget(&hexview);
    w.layout()->addWidget(&cpu_group);

    cpu_group.setLayout(new QVBoxLayout);

    QGroupBox reg_group("Registers", &cpu_group);
    cpu_group.layout()->addWidget(&reg_group);

    QLineEdit reg_A(&reg_group);
    QLineEdit reg_X(&reg_group);
    QLineEdit reg_Y(&reg_group);
    QLineEdit reg_PC(&reg_group);
    QLineEdit reg_SP(&reg_group);

    reg_A.setInputMask("hh");
    reg_X.setInputMask("hh");
    reg_Y.setInputMask("hh");
    reg_PC.setInputMask("hhhh");
    reg_SP.setInputMask("hhhh");

    QLabel reg_A_label("A", &reg_group);
    QLabel reg_X_label("X", &reg_group);
    QLabel reg_Y_label("Y", &reg_group);
    QLabel reg_PC_label("PC", &reg_group);
    QLabel reg_SP_label("SP", &reg_group);

    reg_A_label.setBuddy(&reg_A);
    reg_X_label.setBuddy(&reg_X);
    reg_Y_label.setBuddy(&reg_Y);
    reg_PC_label.setBuddy(&reg_PC);
    reg_SP_label.setBuddy(&reg_SP);

    QGridLayout reg_layout;
    reg_group.setLayout(&reg_layout);
    reg_layout.addWidget(&reg_A, 0, 1);
    reg_layout.addWidget(&reg_X, 1, 1);
    reg_layout.addWidget(&reg_Y, 2, 1);
    reg_layout.addWidget(&reg_PC, 3, 1);
    reg_layout.addWidget(&reg_SP, 4, 1);

    reg_layout.addWidget(&reg_A_label, 0, 0);
    reg_layout.addWidget(&reg_X_label, 1, 0);
    reg_layout.addWidget(&reg_Y_label, 2, 0);
    reg_layout.addWidget(&reg_PC_label, 3, 0);
    reg_layout.addWidget(&reg_SP_label, 4, 0);

    QGroupBox flag_group("Flags", &cpu_group);
    cpu_group.layout()->addWidget(&flag_group);

    QCheckBox flag_C("C", &flag_group);
    QCheckBox flag_Z("Z", &flag_group);
    QCheckBox flag_I("I", &flag_group);
    QCheckBox flag_D("D", &flag_group);
    QCheckBox flag_B("B", &flag_group);
    QCheckBox flag_U("U", &flag_group);
    QCheckBox flag_V("V", &flag_group);
    QCheckBox flag_N("N", &flag_group);

    flag_group.setLayout(new QHBoxLayout);
    flag_group.layout()->addWidget(&flag_C);
    flag_group.layout()->addWidget(&flag_Z);
    flag_group.layout()->addWidget(&flag_I);
    flag_group.layout()->addWidget(&flag_D);
    flag_group.layout()->addWidget(&flag_B);
    flag_group.layout()->addWidget(&flag_U);
    flag_group.layout()->addWidget(&flag_V);
    flag_group.layout()->addWidget(&flag_N);


    QGroupBox control_group("Controls", &cpu_group);
    cpu_group.layout()->addWidget(&control_group);

    control_group.setLayout(new QHBoxLayout);
    QPushButton btn_load("Load File", &control_group);
    QPushButton btn_start("Start", &control_group);
    QPushButton btn_stop("Stop", &control_group);
    QPushButton btn_next("Next", &control_group);
    control_group.layout()->addWidget(&btn_load);
    control_group.layout()->addWidget(&btn_start);
    control_group.layout()->addWidget(&btn_stop);
    control_group.layout()->addWidget(&btn_next);

    QTableWidget tableWidget(12, 1, &cpu_group);
    cpu_group.layout()->addWidget(&tableWidget);
    /**************************/

    /**************************/

    QFile file;

    auto buffer = QByteArray{"puedo escribir los versos más tristes"};

    QObject::connect(&btn_load, &QPushButton::clicked, [&w, &hexview, &file, &buffer]() {
        QString fileName = QFileDialog::getOpenFileName(&w, "Open File", QDir::homePath());
        if (!fileName.isEmpty())
        {
            file.setFileName(fileName);
            QHexDocument *document = QHexDocument::fromMemory<QMemoryBuffer>(buffer);

            hexview.setDocument(document); // Associate QHexEditData with this QHexEdit

            // Document editing
            QByteArray data = document->read(24, 78); // Read 78 bytes starting to offset 24
            document->insert(4, "Hello QHexEdit");    // Insert a string to offset 4
            document->remove(6, 10);                  // Delete bytes from offset 6 to offset 10
            document->replace(30, "New Data");        // Replace bytes from offset 30 with the string "New Data"

            // Metatadata management
            QHexMetadata *hexmetadata = document->metadata();

            hexmetadata->background(6, 0, 10, Qt::red);       // Highlight background to line 6, from 0 to 10
            hexmetadata->foreground(8, 0, 15, Qt::darkBlue);  // Highlight foreground to line 8, from 0 to 15
            hexmetadata->comment(16, 3, 1, "I'm a comment!"); // Add a comment to line 16
            //hexmetadata->clear();
            qDebug() << buffer;
        }
    });

    w.show();

    return a.exec();
}
