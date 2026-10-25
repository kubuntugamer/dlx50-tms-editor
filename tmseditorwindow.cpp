#include "tmseditorwindow.h"

#include <QApplication>
#include <QMainWindow>
#include <QTabWidget>
#include <QTableWidget>
#include <QList>
#include <QMap>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QKeySequence>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QRegularExpression>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <algorithm>


TmsEditorWindow::TmsEditorWindow(const QString& filePath)
    : m_currentFilePath(filePath)
{
    buildUI();
    if (!filePath.isEmpty() && !filePath.isNull()) {
        loadFile(filePath);
    }
}

TmsEditorWindow::~TmsEditorWindow()
{
    m_tabWidget = nullptr;
}

void TmsEditorWindow::buildUI()
{
    setWindowTitle(tr("TMS / DLX‑50‑style Editor"));
    resize(1200, 760);

    // ---------- Per‑section default column counts (used by populateTab) ----------
    m_colCount = {
        {"PLU", 4}, {"DPT", 2}, {"CLS", 2}, {"UNT", 3}, {"BAR", 2},
        {"TMS", 2}, {"TMR", 1}, {"INF", 2}, {"TMT", 2}, {"REP", 2},
        {"SCP", 3}
    };

    // ---------- Tab widget ----------
    // The section tabs themselves are created in loadFile(), once a file is
    // actually opened — building them here only to throw them away on load
    // just wastes work and leaked widgets.
    m_tabWidget = new QTabWidget(this);
    setCentralWidget(m_tabWidget);

    // ---------- File menu ----------
    QMenuBar* menuBar = this->menuBar();
    QMenu* fileMenu = menuBar->addMenu(tr("File"));
    QAction* openAction = fileMenu->addAction(tr("Open…"));
    openAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    connect(openAction, &QAction::triggered, this, &TmsEditorWindow::openFile);

    // Save – DLX‑50 style: back the original up to <file>.BAK, then write
    // the edited tables straight back over the original filename.
    QAction* saveAction = fileMenu->addAction(tr("Save"));
    saveAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    connect(saveAction, &QAction::triggered, this, &TmsEditorWindow::saveFile);

    QAction* saveAsAction = fileMenu->addAction(tr("Save As…"));
    saveAsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S));
    connect(saveAsAction, &QAction::triggered, this, &TmsEditorWindow::saveFileAs);

    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(tr("Exit"));
    exitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    connect(exitAction, &QAction::triggered, this, &QMainWindow::close);

    // ---------- Help menu ----------
    QMenu* helpMenu = menuBar->addMenu(tr("Help"));
    QAction* aboutAction = helpMenu->addAction(tr("About"));
    connect(aboutAction, &QAction::triggered, [this]() {
        QMessageBox::about(this, tr("About"),
            tr("TMS / DLX‑50‑style Editor\n"
               "Reads / writes DETECTO .TMS scale files.\n"
               "Qt C++ (Qt Creator 6) build."));
    });
}

// ----------------------------------------------------------------
bool TmsEditorWindow::loadFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Cannot open file %1:\n%2").arg(path).arg(file.errorString()));
        return false;
    }

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);
    m_rawContent = in.readAll();
    file.close();

    m_currentFilePath = path;
    setWindowFilePath(m_currentFilePath);

    // Clear existing tabs and rebuild them for this file.
    // NOTE: removeTab() only detaches the page — it does NOT delete it, so
    // we must delete the widget ourselves or we leak one table per reload.
    m_tables.clear();
    while (m_tabWidget->count() > 0) {
        QWidget* page = m_tabWidget->widget(0);
        m_tabWidget->removeTab(0);
        delete page;
    }

    // Re-create the section tabs (same code as in buildUI)
    QStringList sectionTags = {
        "PLU", "DPT", "CLS", "UNT", "BAR",
        "TMS", "TMR", "INF", "TMT", "REP", "SCP"
    };

    QMap<QString, int> colCount = {
        {"PLU", 4}, {"DPT", 2}, {"CLS", 2}, {"UNT", 3}, {"BAR", 2},
        {"TMS", 2}, {"TMR", 1}, {"INF", 2}, {"TMT", 2}, {"REP", 2},
        {"SCP", 3}
    };

    for (const QString& tag : sectionTags) {
        int cols = colCount.value(tag, 1);
        QWidget* page = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        QTableWidget* table = new QTableWidget(page);
        table->setAlternatingRowColors(true);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table->setColumnCount(cols);
        table->setRowCount(0);
        // Headers set later in populateTab
        table->setHorizontalHeaderLabels(QStringList(cols, tr("Column")));

        connect(table, &QTableWidget::cellChanged,
                this, [this, table]() { onCellChanged(); });

        layout->addWidget(table);
        page->setLayout(layout);

        QString label = tag;
        label[0] = label[0].toUpper();
        m_tabWidget->addTab(page, label);

        // IMPORTANT: register the freshly created table so that Save reads
        // from the current tables, not from the ones buildUI() made (which
        // were just deleted above).
        m_tables[tag] = table;

        // Populate the table for this section
        populateTab(tag, table);
    }

    statusBar()->showMessage(tr("Loaded: %1").arg(QFileInfo(path).fileName()));
    return true;
}

// ----------------------------------------------------------------
// Return the raw block for a given section (including DWL/END markers)
// ----------------------------------------------------------------
QString TmsEditorWindow::rawSection(const QString& tag)
{
    QString startMarker = QString("DWL\t%1\t").arg(tag);
    QString endMarker   = QString("END\t%1\t").arg(tag);

    int startIdx = m_rawContent.indexOf(startMarker);
    if (startIdx == -1) return QString();

    int endIdx = m_rawContent.indexOf(endMarker, startIdx + startMarker.size());
    if (endIdx == -1) return QString();

    return m_rawContent.mid(startIdx, (endIdx + endMarker.size()) - startIdx);
}

// ----------------------------------------------------------------
// Populate a given table widget with the rows for a particular section
// ----------------------------------------------------------------
void TmsEditorWindow::populateTab(const QString& tag, QTableWidget* table)
{
    const QString block = rawSection(tag);
    if (block.isEmpty()) {
        // No section in the file – leave the table empty but set the appropriate headers.
        QStringList headers;
        int cols = m_colCount.value(tag, 1);

        if (tag == "PLU")
            headers << tr("PLU #") << tr("Product code") << tr("Product name") << tr("Rest (weight/price)");
        else if (tag == "DPT")
            headers << tr("DPT code") << tr("Value / description");
        else if (tag == "CLS")
            headers << tr("CLS code") << tr("Default value");
        else if (tag == "UNT")
            headers << tr("Unit code") << tr("Description") << tr("Factor");
        else if (tag == "BAR")
            headers << tr("Barcode #") << tr("Barcode data");
        else if (tag == "TMS")
            headers << tr("TMS ID") << tr("Value");
        else if (tag == "TMR") {
            headers << tr("TMR data");
            table->setColumnCount(1);
        }
        else if (tag == "INF")
            headers << tr("Field") << tr("Value");
        else if (tag == "TMT")
            headers << tr("Code") << tr("Message");
        else if (tag == "REP")
            headers << tr("REP code") << tr("Data");
        else if (tag == "SCP")
            headers << tr("SCP code") << tr("Subcode") << tr("Value");
        else
            headers << tr("Column");

        table->setColumnCount(cols);
        table->setRowCount(0);
        table->setHorizontalHeaderLabels(headers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        return;
    }

    // Split the raw block into individual lines.
    QStringList lines = block.split('\n', Qt::SkipEmptyParts);

    // Extract only the content lines (skip the DWL/END markers).
    QStringList contentLines;
    for (const QString& line : lines) {
        if (line.startsWith("DWL\t"))
            continue;
        if (line.startsWith("END\t"))
            continue;
        contentLines.append(line);
    }

    int cols = m_colCount.value(tag, 1);
    QVector<QVector<QString>> rowsData;

    if (tag == "PLU") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("PLU\t"))
                continue;

            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QString name;
            int nameIdx = -1;
            for (int i = 2; i < (int)fields.size(); ++i) {
                if (fields[i].contains(QRegularExpression("[A-Za-z]"))) {
                    name = fields[i];
                    nameIdx = i;
                    break;
                }
            }

            QString rest;
            if (nameIdx >= 0 && nameIdx + 1 < (int)fields.size()) {
                rest = fields.mid(nameIdx + 1).join('\t');
            }

            QVector<QString> row;
            row.append(fields.value(0));   // PLU #
            row.append(fields.value(1));   // Product code
            row.append(name);               // Product name
            row.append(rest);               // Rest
            rowsData.append(row);
        }
    } else if (tag == "DPT") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("DPT\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // DPT code
            row.append(fields.mid(1).join('\t'));
            rowsData.append(row);
        }
    } else if (tag == "CLS") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("CLS\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // CLS code
            row.append(fields.mid(1).join('\t'));
            rowsData.append(row);
        }
    } else if (tag == "UNT") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("UNT\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // Unit code
            row.append(fields.value(1));   // Description
            row.append(fields.value(2));   // Factor
            rowsData.append(row);
        }
    } else if (tag == "BAR") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("BAR\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // Barcode number
            row.append(fields.mid(1).join('\t'));
            rowsData.append(row);
        }
    } else if (tag == "TMS") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("TMS\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // TMS ID
            row.append(fields.value(1));   // Value
            rowsData.append(row);
        }
    } else if (tag == "TMR") {
        for (const QString& line : contentLines) {
            QVector<QString> row;
            row.append(line);
            rowsData.append(row);
        }
    } else if (tag == "INF") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("INF\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // field name
            row.append(fields.mid(1).join('\t'));
            rowsData.append(row);
        }
    } else if (tag == "TMT") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("TMT\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // code
            row.append(fields.mid(1).join('\t'));
            rowsData.append(row);
        }
    } else if (tag == "REP") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("REP\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // REP code
            row.append(fields.mid(1).join('\t'));
            rowsData.append(row);
        }
    } else if (tag == "SCP") {
        for (const QString& line : contentLines) {
            if (!line.startsWith("SCP\t"))
                continue;
            QStringList fields = line.split('\t', Qt::SkipEmptyParts);

            QVector<QString> row;
            row.append(fields.value(0));   // SCP code
            row.append(fields.value(1));   // Subcode
            row.append(fields.mid(2).join('\t'));
            rowsData.append(row);
        }
    }

    // Set up the headers for this tab.
    QStringList headers;
    if (tag == "PLU")
        headers << tr("PLU #") << tr("Product code") << tr("Product name") << tr("Rest (weight/price)");
    else if (tag == "DPT")
        headers << tr("DPT code") << tr("Value / description");
    else if (tag == "CLS")
        headers << tr("CLS code") << tr("Default value");
    else if (tag == "UNT")
        headers << tr("Unit code") << tr("Description") << tr("Factor");
    else if (tag == "BAR")
        headers << tr("Barcode #") << tr("Barcode data");
    else if (tag == "TMS")
        headers << tr("TMS ID") << tr("Value");
    else if (tag == "TMR")
        headers << tr("TMR data");
    else if (tag == "INF")
        headers << tr("Field") << tr("Value");
    else if (tag == "TMT")
        headers << tr("Code") << tr("Message");
    else if (tag == "REP")
        headers << tr("REP code") << tr("Data");
    else if (tag == "SCP")
        headers << tr("SCP code") << tr("Subcode") << tr("Value");
    else
        headers << tr("Column");

    int finalCols = headers.size();
    table->setColumnCount(finalCols);
    table->setHorizontalHeaderLabels(headers);          // <-- was missing
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    int rowCount = rowsData.size();
    table->setRowCount(rowCount);

    for (int r = 0; r < rowCount; ++r) {
        for (int c = 0; c < finalCols; ++c) {
            QString val = (c < (int)rowsData[r].size()) ? rowsData[r][c] : QString();
            QTableWidgetItem* item = new QTableWidgetItem(val);
            table->setItem(r, c, item);
        }
    }
}

// ----------------------------------------------------------------
// Placeholder for cell changes – in a full version we would store
// a per‑section delta so that Save can rebuild the file.
// ----------------------------------------------------------------
void TmsEditorWindow::onCellChanged()
{
    // Currently a no‑op placeholder.  In a full version we would notify
    // the model that the data has changed and that a Save will write back
    // the modified content.
}

// ----------------------------------------------------------------
// File -> Open
// ----------------------------------------------------------------
void TmsEditorWindow::openFile()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open TMS file"),
        QString(),
        tr("DetetcTo / Artcraft files (*.TMS);;All files (*)")
    );
    if (path.isNull() || path.isEmpty())
        return;

    loadFile(path);
}

// ----------------------------------------------------------------
// Back up an existing file to <path>.BAK (overwriting any previous
// backup) and return the backup path, or an empty string on failure.
// ----------------------------------------------------------------
static QString backupToBak(const QString& path)
{
    if (!QFile::exists(path))
        return QString();

    const QString bakPath = path + ".BAK";

    // Remove any stale backup so the copy below always succeeds.
    if (QFile::exists(bakPath))
        QFile::remove(bakPath);

    if (!QFile::copy(path, bakPath))
        return QString();

    return bakPath;
}

// ----------------------------------------------------------------
// Write the given content to the given path.
// ----------------------------------------------------------------
static bool writeTextFile(const QString& path, const QString& content,
                          QString* errorOut)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << content;
    file.close();
    return true;
}

// ----------------------------------------------------------------
// File -> Save  (DLX‑50 behaviour: back up, then overwrite in place)
// ----------------------------------------------------------------
void TmsEditorWindow::saveFile()
{
    if (m_currentFilePath.isEmpty()) {
        // Nothing loaded yet – fall back to Save As.
        saveFileAs();
        return;
    }

    // Rebuild the raw file content from the current table contents,
    // preserving the original order of sections and all non‑TMS lines
    // that appear outside the DWL…END blocks.
    QString updatedContent = rebuildFullContentFromTables();

    // 1) Back the current on‑disk file up to <file>.BAK before touching it.
    const QString bakPath = backupToBak(m_currentFilePath);

    // 2) Write the edited content straight back over the original filename.
    QString err;
    if (!writeTextFile(m_currentFilePath, updatedContent, &err)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Cannot write to file %1:\n%2").arg(m_currentFilePath).arg(err));
        return;
    }

    m_rawContent = updatedContent;

    QString msg = tr("Saved: %1").arg(QFileInfo(m_currentFilePath).fileName());
    if (!bakPath.isEmpty())
        msg += tr("  (backup: %1)").arg(QFileInfo(bakPath).fileName());
    statusBar()->showMessage(msg);
}

// ----------------------------------------------------------------
// File -> Save As...
// ----------------------------------------------------------------
void TmsEditorWindow::saveFileAs()
{
    QString path = QFileDialog::getSaveFileName(
        this,
        tr("Save TMS file"),
        m_currentFilePath,
        tr("DetetcTo / Artcraft files (*.TMS);;All files (*)")
    );
    if (path.isNull() || path.isEmpty())
        return;

    QString updatedContent = rebuildFullContentFromTables();

    // If we are overwriting an existing file, keep the same safety net.
    QString bakPath;
    if (QFile::exists(path))
        bakPath = backupToBak(path);

    QString err;
    if (!writeTextFile(path, updatedContent, &err)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Cannot write to file %1:\n%2").arg(path).arg(err));
        return;
    }

    m_rawContent = updatedContent;
    m_currentFilePath = path;
    setWindowFilePath(m_currentFilePath);

    QString msg = tr("Saved: %1").arg(QFileInfo(path).fileName());
    if (!bakPath.isEmpty())
        msg += tr("  (backup: %1)").arg(QFileInfo(bakPath).fileName());
    statusBar()->showMessage(msg);
}

// ----------------------------------------------------------------
// Rebuild the full .TMS file content from the current table state.
// Sections are emitted in the fixed order used by the UI; everything
// outside the DWL/END blocks is preserved byte‑for‑byte.
// ----------------------------------------------------------------
QString TmsEditorWindow::rebuildFullContentFromTables()
{
    // Work on a mutable copy of the current raw file so that every line
    // that is not part of a recognised DWL…END block survives unchanged.
    QString content = m_rawContent;

    // Ordered list of sections supported by the UI.
    static const QList<QString> sectionTags = {
        "PLU", "DPT", "CLS", "UNT", "BAR",
        "TMS", "TMR", "INF", "TMT", "REP", "SCP"
    };

    // For each section, replace the raw DWL…END block with the current
    // table contents (if a table for that section exists and the file
    // originally contained that section).
    for (const QString& tag : sectionTags) {
        QTableWidget* table = m_tables.value(tag, nullptr);
        if (!table)
            continue;

        QString startMarker = QString("DWL\t%1\t").arg(tag);
        QString endMarker   = QString("END\t%1\t").arg(tag);

        int startIdx = content.indexOf(startMarker);
        if (startIdx == -1)
            continue;

        int endIdx = content.indexOf(endMarker, startIdx + startMarker.size());
        if (endIdx == -1)
            continue;

        // Reconstruct the block from the table.
        QString newBlock = startMarker;
        int rowCount = table->rowCount();
        int colCount = table->columnCount();

        for (int r = 0; r < rowCount; ++r) {
            // Emit the section‑specific line prefix.
            QString linePrefix;
            if (tag == "PLU")
                linePrefix = "PLU\t";
            else if (tag == "DPT")
                linePrefix = "DPT\t";
            else if (tag == "CLS")
                linePrefix = "CLS\t";
            else if (tag == "UNT")
                linePrefix = "UNT\t";
            else if (tag == "BAR")
                linePrefix = "BAR\t";
            else if (tag == "TMS")
                linePrefix = "TMS\t";
            else if (tag == "TMR")
                linePrefix = "TMR\t";
            else if (tag == "INF")
                linePrefix = "INF\t";
            else if (tag == "TMT")
                linePrefix = "TMT\t";
            else if (tag == "REP")
                linePrefix = "REP\t";
            else if (tag == "SCP")
                linePrefix = "SCP\t";
            else
                linePrefix = QString();

            QStringList fields;
            for (int c = 0; c < colCount; ++c) {
                QTableWidgetItem* item = table->item(r, c);
                fields << (item ? item->text() : QString());
            }

            QString rowText = linePrefix + fields.join("\t");
            newBlock += rowText + "\r\n";
        }

        newBlock += endMarker;

        // Slice out the old block (including the END line) and replace it.
        int oldBlockStart = startIdx;
        int oldBlockEnd   = endIdx + endMarker.size();
        content.replace(oldBlockStart, oldBlockEnd - oldBlockStart, newBlock);
    }

    return content;
}

// NOTE: no "#include "tmseditorwindow.moc"" here — the Q_OBJECT class
// (TmsEditorWindow) is declared in the header, so AUTOMOC generates its
// moc file from tmseditorwindow.h, not from this .cpp.
