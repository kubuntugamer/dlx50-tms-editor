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
#include <QLineEdit>
#include <QKeyEvent>
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
namespace {

struct SectionRow {
    QStringList tokens;   // full row token list, section tag kept once
    int offset = 0;       // 1 when tokens[0] is the section tag
};

QString sectionStartMarker(const QString& tag) { return QString("DWL\t%1").arg(tag); }
QString sectionEndMarker(const QString& tag)   { return QString("END\t%1").arg(tag); }

// Locate "DWL<TAB>TAG" / "END<TAB>TAG" without assuming what follows the tag:
// older output ended the marker with a tab, this one ends it with a newline,
// and both have to be understood.
int indexOfSectionMarker(const QString& text, const QString& marker, int from)
{
    int pos = from;
    while ((pos = text.indexOf(marker, pos)) != -1) {
        const int after = pos + marker.size();
        if (after >= text.size())
            return pos;
        const QChar c = text.at(after);
        if (c == '\t' || c == '\r' || c == '\n')
            return pos;
        pos = after;
    }
    return -1;
}

// Split a "DWL<TAB>TAG ... END<TAB>TAG" block into its data rows.  The markers
// are removed from the text instead of skipped line by line, because the first
// data row can share a line with the DWL marker.
QVector<SectionRow> parseSectionBlock(const QString& block, const QString& tag)
{
    const QString startMarker = sectionStartMarker(tag);
    const QString endMarker   = sectionEndMarker(tag);

    QString body = block;
    if (body.startsWith(startMarker))
        body.remove(0, startMarker.size());
    if (body.startsWith('\t'))          // "DWL\tTAG\t"
        body.remove(0, 1);
    if (body.startsWith('\r'))
        body.remove(0, 1);
    if (body.startsWith('\n'))
        body.remove(0, 1);

    if (body.endsWith(endMarker))
        body.chop(endMarker.size());
    if (body.endsWith('\t'))            // "END\tTAG\t"
        body.chop(1);
    if (body.endsWith('\r'))
        body.chop(1);
    if (body.endsWith('\n'))
        body.chop(1);

    QVector<SectionRow> rows;
    const QStringList lines = body.split('\n');
    for (QString line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue;

        QStringList tokens = line.split('\t', Qt::KeepEmptyParts);
        // Older output repeated the section tag at the start of the row.
        while (tokens.size() > 1 && tokens.at(0) == tag && tokens.at(1) == tag)
            tokens.removeFirst();

        SectionRow row;
        row.tokens = tokens;
        row.offset = (!tokens.isEmpty() && tokens.first() == tag) ? 1 : 0;
        rows.append(row);
    }
    return rows;
}

} // namespace

QString TmsEditorWindow::rawSection(const QString& tag)
{
    const QString startMarker = QString("DWL\t%1").arg(tag);
    const QString endMarker   = QString("END\t%1").arg(tag);

    const int startIdx = indexOfSectionMarker(m_rawContent, startMarker, 0);
    if (startIdx == -1)
        return QString();

    const int endIdx = indexOfSectionMarker(m_rawContent, endMarker,
                                            startIdx + startMarker.size());
    if (endIdx == -1)
        return QString();

    return m_rawContent.mid(startIdx, (endIdx + endMarker.size()) - startIdx);
}

// ----------------------------------------------------------------
// Populate a given table widget with the rows for a particular section
// ----------------------------------------------------------------
void TmsEditorWindow::populateTab(const QString& tag, QTableWidget* table)
{
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

    const int cols = qMax(1, m_colCount.value(tag, headers.size()));

    table->setColumnCount(cols);
    table->setRowCount(0);
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);

    m_rowTokens.remove(tag);
    m_rowOffsets.remove(tag);
    m_rowNameIdx.remove(tag);

    const QString block = rawSection(tag);
    if (block.isEmpty())
        return;

    const QVector<SectionRow> rows = parseSectionBlock(block, tag);

    QVector<QStringList> tokenRows;
    QVector<int> offsets;
    QVector<int> nameIndexes;
    QVector<QVector<QString>> displayRows;

    for (const SectionRow& row : rows) {
        tokenRows.append(row.tokens);
        offsets.append(row.offset);

        QVector<QString> display;

        if (tag == "PLU") {
            // PLU # and product code are the first two tokens after the tag;
            // the product name is the first token holding letters (everything
            // before it is numeric codes, prices and weights).
            const int base = row.offset;
            int nameIdx = -1;
            for (int i = base + 1; i < row.tokens.size(); ++i) {
                if (row.tokens.at(i).contains(QRegularExpression("[A-Za-z]"))) {
                    nameIdx = i;
                    break;
                }
            }
            nameIndexes.append(nameIdx);

            const QString number = row.tokens.value(base);
            const QString code   = (nameIdx == base + 1) ? QString()
                                                         : row.tokens.value(base + 1);
            const QString name   = (nameIdx >= 0) ? row.tokens.at(nameIdx)
                                                  : row.tokens.value(base + 1);
            QString rest;
            if (nameIdx >= 0 && nameIdx + 1 < row.tokens.size())
                rest = row.tokens.mid(nameIdx + 1).join('\t');

            display << number << code << name << rest;
        } else {
            nameIndexes.append(-1);
            for (int c = 0; c < cols; ++c)
                display << row.tokens.value(row.offset + c);
        }

        displayRows.append(display);
    }

    // Keep the complete token rows so Save can write back the fields that have
    // no column of their own.
    m_rowTokens[tag]  = tokenRows;
    m_rowOffsets[tag] = offsets;
    m_rowNameIdx[tag] = nameIndexes;

    table->setRowCount(displayRows.size());
    for (int r = 0; r < displayRows.size(); ++r) {
        for (int c = 0; c < cols; ++c)
            table->setItem(r, c, new QTableWidgetItem(displayRows.at(r).value(c)));
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
// A QTableWidget keeps freshly typed text in its cell editor until that
// editor closes; saving at that moment would write the previous value.  Drop
// the focus so the delegate commits what the user just typed.
static void commitOpenCellEditors(const QMap<QString, QTableWidget*>& tables)
{
    // Any visible line edit inside a table is an open cell editor.  Sending
    // Return to it makes the item delegate commit the typed text into the
    // model, which is what Save needs to see.
    for (QTableWidget* table : tables) {
        if (!table)
            continue;
        const QList<QLineEdit*> editors = table->findChildren<QLineEdit*>();
        for (QLineEdit* editor : editors) {
            if (!editor->isVisible())
                continue;
            QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(editor, &commit);
        }
    }

    // Editors of other kinds commit when they lose focus.
    if (QWidget* focus = QApplication::focusWidget()) {
        if (!qobject_cast<QLineEdit*>(focus) && focus->inherits("QWidget"))
            focus->clearFocus();
    }

    QCoreApplication::processEvents();
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

    // Commit a cell that is still being edited, otherwise its text is not in
    // the table yet and would be dropped from the file.
    commitOpenCellEditors(m_tables);

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
    commitOpenCellEditors(m_tables);

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
    QString content = m_rawContent;

    // Every block that exists in the file is re-serialised, including the ones
    // without a tab of their own, so no section header can stay glued to the
    // data that used to follow it on the same line.
    static const QStringList allTags = {
        "PLU", "DPT", "CLS", "UNT", "BAR", "LAB", "SAL",
        "REP", "SCF", "SCP", "TMS", "TMT", "TMR", "INF", "TIM", "BMP"
    };

    for (const QString& tag : allTags) {
        const QString startMarker = sectionStartMarker(tag);
        const QString endMarker   = sectionEndMarker(tag);

        int startIdx = indexOfSectionMarker(content, startMarker, 0);
        if (startIdx == -1)
            continue;

        const int endIdx = indexOfSectionMarker(content, endMarker,
                                                startIdx + startMarker.size());
        if (endIdx == -1)
            continue;

        const int blockEnd = endIdx + endMarker.size();

        // Rows come from what was loaded (so the fields without a column are
        // intact); blocks with no tab are parsed straight from the text.
        QVector<SectionRow> rows;
        if (m_rowTokens.contains(tag)) {
            const QVector<QStringList> stored = m_rowTokens.value(tag);
            const QVector<int> offsets = m_rowOffsets.value(tag);
            for (int i = 0; i < stored.size(); ++i) {
                SectionRow row;
                row.tokens = stored.at(i);
                row.offset = (i < offsets.size()) ? offsets.at(i) : 0;
                rows.append(row);
            }
        } else {
            rows = parseSectionBlock(content.mid(startIdx, blockEnd - startIdx), tag);
        }

        // Overlay whatever was edited in the table.
        QTableWidget* table = m_tables.value(tag, nullptr);
        if (table) {
            const QVector<int> nameIndexes = m_rowNameIdx.value(tag);

            for (int r = 0; r < rows.size() && r < table->rowCount(); ++r) {
                SectionRow& row = rows[r];

                if (tag == "PLU") {
                    const int nameIdx = (r < nameIndexes.size()) ? nameIndexes.at(r) : -1;

                    if (QTableWidgetItem* item = table->item(r, 0)) {
                        const int idx = row.offset;              // PLU #
                        if (idx < row.tokens.size())
                            row.tokens[idx] = item->text();
                    }
                    if (QTableWidgetItem* item = table->item(r, 1)) {
                        const int idx = row.offset + 1;          // product code
                        if (nameIdx != idx && idx < row.tokens.size())
                            row.tokens[idx] = item->text();
                    }
                    if (QTableWidgetItem* item = table->item(r, 2)) {
                        if (nameIdx >= 0 && nameIdx < row.tokens.size())
                            row.tokens[nameIdx] = item->text();
                    }
                    if (QTableWidgetItem* item = table->item(r, 3)) {
                        if (nameIdx >= 0) {
                            const QString original = row.tokens.mid(nameIdx + 1).join('\t');
                            if (item->text() != original) {
                                row.tokens = row.tokens.mid(0, nameIdx + 1)
                                           + item->text().split('\t', Qt::KeepEmptyParts);
                            }
                        }
                    }
                } else {
                    for (int c = 0; c < table->columnCount(); ++c) {
                        const int idx = row.offset + c;
                        if (idx >= row.tokens.size())
                            break;
                        if (QTableWidgetItem* item = table->item(r, c))
                            row.tokens[idx] = item->text();
                    }
                }
            }
        }

        // Drop stray tabs/spaces an older layout left in front of the header,
        // and make sure the header actually begins a line.
        while (startIdx > 0
               && (content.at(startIdx - 1) == '\t' || content.at(startIdx - 1) == ' ')) {
            --startIdx;
        }

        // "DWL<TAB>TAG" alone on its line, one data row per line with single
        // tabs between fields, then "END<TAB>TAG" alone, then one blank line.
        QString newBlock;
        if (startIdx > 0 && content.at(startIdx - 1) != '\n')
            newBlock += '\n';
        newBlock += startMarker + "\n";
        for (const SectionRow& row : rows)
            newBlock += row.tokens.join('\t') + "\n";
        newBlock += endMarker + "\n\n";

        int afterIdx = blockEnd;
        while (afterIdx < content.size()
               && (content.at(afterIdx) == '\t' || content.at(afterIdx) == ' '
                   || content.at(afterIdx) == '\r' || content.at(afterIdx) == '\n')) {
            ++afterIdx;
        }

        content.replace(startIdx, afterIdx - startIdx, newBlock);
    }

    // Document closer: "END<TAB>ECS" alone on the last line.
    content.replace(QRegularExpression("END\tECS[ \t]*(?:\r?\n)*\\z"),
                    QString("END\tECS\n"));

    // Exactly one blank line between blocks, and a terminated last line.
    content.replace(QRegularExpression("\n{3,}"), "\n\n");
    if (!content.endsWith('\n'))
        content += '\n';

    return content;
}
