#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDebug>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "external/QHexView/document/buffer/qmemorybuffer.h"
#include "external/QHexView/qhexview.h"
#include "include/bus.h"
#include "include/instruction.h"

int main(int argc, char* argv[])
{
    QApplication a(argc, argv);

    QWidget w;
    w.setLayout(new QHBoxLayout);

    QHexView hexview(&w);
    QGroupBox cpu_group(&w);
    w.layout()->addWidget(&hexview);
    w.layout()->addWidget(&cpu_group);

    cpu_group.setLayout(new QVBoxLayout);

    QGroupBox instruction_group("Instruction", &cpu_group);
    cpu_group.layout()->addWidget(&instruction_group);
    instruction_group.setLayout(new QHBoxLayout);
    QLineEdit instruction(&instruction_group);
    instruction.setReadOnly(true);
    QLineEdit operand(&instruction_group);
    operand.setReadOnly(true);
    QLabel instruction_label("OP", &instruction_group);
    instruction_group.layout()->addWidget(&instruction_label);
    instruction_group.layout()->addWidget(&instruction);
    instruction_group.layout()->addWidget(&operand);

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
    flag_group.layout()->addWidget(&flag_N);
    flag_group.layout()->addWidget(&flag_V);
    flag_group.layout()->addWidget(&flag_U);
    flag_group.layout()->addWidget(&flag_B);
    flag_group.layout()->addWidget(&flag_D);
    flag_group.layout()->addWidget(&flag_I);
    flag_group.layout()->addWidget(&flag_Z);
    flag_group.layout()->addWidget(&flag_C);

    QGroupBox control_group("Controls", &cpu_group);
    cpu_group.layout()->addWidget(&control_group);

    control_group.setLayout(new QHBoxLayout);
    QPushButton btn_load("Load File", &control_group);
    QPushButton btn_go("Go", &control_group);
    QPushButton btn_stop("Stop", &control_group);
    QPushButton btn_next("Next", &control_group);
    QPushButton btn_reset("Reset", &control_group);
    QCheckBox trace("Trace", &control_group);

    control_group.layout()->addWidget(&btn_load);
    control_group.layout()->addWidget(&btn_go);
    control_group.layout()->addWidget(&btn_stop);
    control_group.layout()->addWidget(&btn_next);
    control_group.layout()->addWidget(&btn_reset);
    control_group.layout()->addWidget(&btn_reset);
    control_group.layout()->addWidget(&trace);

    QTableWidget tableWidget(256, 2, &cpu_group);
    cpu_group.layout()->addWidget(&tableWidget);
    /**************************/

    /**************************/
    Bus console;
    QObject::connect(&btn_load, &QPushButton::clicked, [&w, &hexview, &console]() {
        QString fileName = QFileDialog::getOpenFileName(&w, "Open File", QDir::homePath());

        if (fileName.isEmpty()) {
            return;
        }

        bool ok;
        uint16_t addr = QInputDialog::getInt(&w, "Target address", "#", 0, 0, 64 * 1024, 1, &ok);

        if (!ok) {
            return;
        }

        QFile file(fileName);
        file.open(QIODevice::ReadOnly);
        file.read(reinterpret_cast<char*>(console.ram.memory.data()) + addr, console.ram.memory.size() - addr);

        QHexDocument* document = QHexDocument::fromMemory<QMemoryBuffer>(
            reinterpret_cast<char*>(console.ram.memory.data()), console.ram.memory.size());

        hexview.setDocument(document);  // Associate QHexEditData with this QHexEdit
        document->metadata()->background(CPU::STACK_BASE_ADDR / HEX_LINE_LENGTH, CPU::STACK_BASE_ADDR % HEX_LINE_LENGTH,
                                         1, Qt::green);

        /*
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
        */
        console.cpu.reset();
    });

    QObject::connect(&btn_next, &QPushButton::clicked, [&] {
        while (!console.cpu.clock(true)) {
            ;
        }
    });
    /*
    QObject::connect(&btn_stop, &QPushButton::clicked, [&] {
        while (true) {
#if 0
            QList<QTableWidgetItem *> items = tableWidget.findItems(QString::number(console.cpu.registers.PC, 16), Qt::MatchFixedString);
            if (items.isEmpty()) {
                break;
            }
#endif
            auto previous_pc = console.cpu.registers.PC;
            auto executed = console.cpu.clock(false);

            if (executed && (previous_pc == console.cpu.registers.PC)) {
                std::cout << "TRAP " << std::hex << previous_pc << std::endl;
                console.cpu.signal_update();
                break;
            }
        }
    });
*/
    auto update_gui = [&]() {
        reg_A.setText(QString::number(console.cpu.registers.A, 16));
        reg_X.setText(QString::number(console.cpu.registers.X, 16));
        reg_Y.setText(QString::number(console.cpu.registers.Y, 16));
        reg_PC.setText(QString::number(console.cpu.registers.PC, 16));
        reg_SP.setText(QString::number(console.cpu.registers.SP, 16));
        flag_I.setChecked(console.cpu.get_flag(CPU::FLAGS::I));
        flag_B.setChecked(console.cpu.get_flag(CPU::FLAGS::B));
        flag_C.setChecked(console.cpu.get_flag(CPU::FLAGS::C));
        flag_D.setChecked(console.cpu.get_flag(CPU::FLAGS::D));
        flag_N.setChecked(console.cpu.get_flag(CPU::FLAGS::N));
        flag_U.setChecked(console.cpu.get_flag(CPU::FLAGS::U));
        flag_V.setChecked(console.cpu.get_flag(CPU::FLAGS::V));
        flag_Z.setChecked(console.cpu.get_flag(CPU::FLAGS::Z));
        instruction.setText(
            QString::fromStdString(Instruction::instruction_set[console.cpu.read(console.cpu.registers.PC)].name));
        //operand.setText(QString::number(console.cpu.fetched_operand, 16));

        QHexDocument* document =
            QHexDocument::fromMemory<QMemoryBuffer>((char*)console.ram.memory.data(), console.ram.memory.size());
        hexview.setDocument(document);

        document->metadata()->background(CPU::STACK_BASE_ADDR / HEX_LINE_LENGTH, CPU::STACK_BASE_ADDR % HEX_LINE_LENGTH,
                                         1, Qt::green);
        document->metadata()->background((CPU::STACK_BASE_ADDR + console.cpu.registers.SP) / HEX_LINE_LENGTH,
                                         (CPU::STACK_BASE_ADDR + console.cpu.registers.SP) % HEX_LINE_LENGTH, 1,
                                         Qt::blue);
        document->metadata()->background(console.cpu.registers.PC / HEX_LINE_LENGTH,
                                         console.cpu.registers.PC % HEX_LINE_LENGTH, 1, Qt::red);
        document->cursor()->moveTo(console.cpu.registers.PC);

        /*
        for (int i = 0; i <= 0xff; i++) {
            QTableWidgetItem *newItem = new QTableWidgetItem(QString::number(console.cpu.read(CPU::STACK_BASE_ADDR + 0xff - i), 16));
            if (console.cpu.registers.SP == 0xff - i) {
                newItem->setBackgroundColor(Qt::red);
            }
            newItem->setFlags(newItem->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsSelectable);
            tableWidget.setItem(i, 1, newItem);
        }*/
    };

    console.cpu.register_update_signal_callback(update_gui);
    QObject::connect(&flag_C, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::C, flag_C.isChecked()); });
    QObject::connect(&flag_Z, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::Z, flag_Z.isChecked()); });
    QObject::connect(&flag_I, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::I, flag_I.isChecked()); });
    QObject::connect(&flag_D, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::D, flag_D.isChecked()); });
    QObject::connect(&flag_B, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::B, flag_B.isChecked()); });
    QObject::connect(&flag_U, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::U, flag_U.isChecked()); });
    QObject::connect(&flag_V, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::V, flag_V.isChecked()); });
    QObject::connect(&flag_N, &QCheckBox::clicked, [&]() { console.cpu.set_flag(CPU::FLAGS::N, flag_N.isChecked()); });

    QTimer tick;

    auto run = [&] {
        auto previous_pc = console.cpu.registers.PC;
        auto executed = console.cpu.clock(trace.isChecked());

        if (executed && (previous_pc == console.cpu.registers.PC)) {
            tick.stop();
            update_gui();
            std::cout << "TRAP " <<std::hex << (unsigned) previous_pc << std::endl;
        }

        /*
        QList<QTableWidgetItem *> items = tableWidget.findItems(QString::number(console.cpu.registers.PC, 16), Qt::MatchFixedString);
        if (!items.isEmpty()) {
            tick.stop();
        }
        */
    };

    QObject::connect(&tick, &QTimer::timeout, run);

    QObject::connect(&btn_stop, &QPushButton::clicked, [&tick] { tick.stop(); });
    QObject::connect(&btn_go, &QPushButton::clicked, [&tick] { tick.start(0); });

    QObject::connect(&btn_reset, &QPushButton::clicked, [&console, &tick] {
        tick.stop();
        console.cpu.reset();
    });

    w.show();
    return a.exec();
}
