#include "tmseditorwindow.h"

#include "updater.h"

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
#include <QVBoxLayout>
#include <QPushButton>
#include <QHBoxLayout>
#include <QTimer>
#include <algorithm>


TmsEditorWindow::TmsEditorWindow(const QString& filePath)
    : m_currentFilePath(filePath)
{
    buildUI();
    if (!filePath.isEmpty() && !filePath.isNull()) {
        loadFile(filePath);
    }

    // Optional self-update probe (issue #3): a few seconds after startup,
    // silently ask GitHub Releases whether a newer build exists. Skipped
    // when --no-update-check is on the command line or the user chose to
    // skip earlier; the dialog has an explicit "skip and stop asking" choice.
    startUpdateCheck();
}

void TmsEditorWindow::startUpdateCheck()
{
    if (!Updater::startupCheckAllowed())
        return;

    QTimer::singleShot(3000, this, [this]() {
        if (!m_updater)
            m_updater = new Updater(this);
        m_updater->checkForUpdates(nullptr, false);
    });
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

    QAction* updateAction = helpMenu->addAction(tr("Check for Updates…"));
    connect(updateAction, &QAction::triggered, this, [this]() {
        if (!m_updater)
            m_updater = new Updater(this);
        m_updater->checkForUpdates(this, true);
    });    QAction* aboutAction = helpMenu->addAction(tr("About"));
    connect(aboutAction, &QAction::triggered, [this]() {
        QMessageBox::about(this, tr("About"),
            tr("TMS / DLX‑50‑style Editor v%1\n"
               "Reads / writes DETECTO .TMS scale files.\n"
               "Qt C++ (Qt Creator 6) build.")
               .arg(Updater::currentVersion()));
    });
}

// ----------------------------------------------------------------
bool TmsEditorWindow::loadFile(const QString& path)
{
    // Read RAW bytes — no QIODevice::Text, which would silently convert the
    // device's CRLF line endings to bare LF. The byte-exact form (CRLF + a
    // trailing tab on every line) is what the DLX-50 hardware accepts, so
    // every untouched byte outside the edited blocks must survive a
    // load→save round trip unchanged.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Error"),
            tr("Cannot open file %1:\n%2").arg(path).arg(file.errorString()));
        return false;
    }

    m_rawContent = QString::fromUtf8(file.readAll());
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

        // ---------- Add / delete rows (issue #4) ----------
        QHBoxLayout* rowButtons = new QHBoxLayout();
        rowButtons->setContentsMargins(4, 4, 4, 0);
        QPushButton* addBtn = new QPushButton(tr("Add row"), page);
        QPushButton* delBtn = new QPushButton(tr("Delete selected"), page);
        addBtn->setToolTip(tr("Adds a new row cloned from the last one (PLU: next free number, name = NEW PRODUCT). Remember to Save."));
        delBtn->setToolTip(tr("Removes every selected row from the section."));
        rowButtons->addWidget(addBtn);
        rowButtons->addWidget(delBtn);
        rowButtons->addStretch(1);
        connect(addBtn, &QPushButton::clicked,
                this, [this, tag]() { addRowToSection(tag); });
        connect(delBtn, &QPushButton::clicked,
                this, [this, tag]() { removeSelectedRows(tag); });

        layout->addLayout(rowButtons);
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
    m_fileLoaded = true;
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

// Index of the PLU name token inside a row's tokens: the first token after
// the number/code prefix that contains letters (everything before it is
// numeric codes, prices and weights).
int findPluNameIdx(const QStringList& tokens, int base)
{
    for (int i = base + 1; i < tokens.size(); ++i) {
        if (tokens.at(i).contains(QRegularExpression("[A-Za-z]")))
            return i;
    }
    return -1;
}

// The table-cell texts for a stored token row — the single source of truth
// shared by populateTab (loading), addRowToSection (new row drafts) and the
// Save overlay, so the three can never drift apart.
QVector<QString> buildDisplayRow(const QString& tag, const QStringList& tokens,
                                 int offset, int cols, int nameIdx)
{
    QVector<QString> display;

    if (tag == "PLU") {
        const int base = offset;
        const QString number = tokens.value(base);
        const QString code   = (nameIdx == base + 1) ? QString()
                                                     : tokens.value(base + 1);
        const QString name   = (nameIdx >= 0) ? tokens.at(nameIdx)
                                              : tokens.value(base + 1);
        QString rest;
        if (nameIdx >= 0 && nameIdx + 1 < tokens.size())
            rest = tokens.mid(nameIdx + 1).join('\t');

        display << number << code << name << rest;
    } else if (tag == "BAR") {
        // Real DLX-50 BAR rows carry the barcode string in a later field
        // (index 6 in the file we inspected: BAR\tidx\t0\t0\t0\t0\tBARCODE\r),
        // with fields 2-5 as hidden numeric placeholders. Show the row index
        // as "Barcode #" and the barcode string as "Barcode data" so the user
        // can actually see and edit the barcode — the hidden fields are
        // preserved untouched through load/save (like PLU's weight/price
        // fields) and survive a round-trip because the save overlay only
        // touches the two displayed columns.
        // The file we inspected stores BAR rows as
        //   BAR\t<idx>\t0\t0\t0\t0\t<barcode>\r
        // and parseSectionBlock strips the leading "BAR" tag, so the stored
        // tokens are [<idx>, 0, 0, 0, 0, <barcode>].  Show token[0] (the row
        // index) as "Barcode #" and token[5] (the barcode string) as
        // "Barcode data".  Fields 1-4 are hidden numeric placeholders that
        // survive a round-trip because the save overlay below only touches
        // tokens 0 and 5.
        display << tokens.value(offset);                           // Barcode # (row index)
        display << (tokens.size() > offset + 5 ? tokens.value(offset + 5) : QString());  // Barcode data
    } else {
        for (int c = 0; c < cols; ++c)
            display << tokens.value(offset + c);
    }
    return display;
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

        if (tag == "PLU")
            nameIndexes.append(findPluNameIdx(row.tokens, row.offset));
        else
            nameIndexes.append(-1);

        displayRows.append(buildDisplayRow(tag, row.tokens, row.offset, cols,
                                           nameIndexes.constLast()));
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
// Issue #4: add a new product/row by cloning the last row of the section
// as an editable draft. Cloning keeps the scale's expected field count for
// the row (PLU rows carry ~60 numeric fields); the identity fields are set
// so the row is safe to save and easy to spot: PLU gets the next free
// number and a NEW PRODUCT name, other sections get the next free index.
// ----------------------------------------------------------------
void TmsEditorWindow::addRowToSection(const QString& tag)
{
    QTableWidget* table = m_tables.value(tag, nullptr);
    if (!table)
        return;

    QVector<QStringList>& tokenRows = m_rowTokens[tag];
    if (tokenRows.isEmpty()) {
        // Nothing to clone — bootstrap the first row where that is safe.
        // PLU needs ~60 numeric fields with the scale's exact semantics;
        // inventing one on the spot is not safe, so it stays clone-only.
        if (tag == "PLU") {
            statusBar()->showMessage(
                tr("PLU has no rows to clone and cannot be bootstrapped — "
                   "open a file that already has PLU entries"));
            return;
        }
        QStringList row{ tag, QStringLiteral("0") };
        tokenRows.append(row);
        m_rowOffsets[tag].append(1);
        m_rowNameIdx[tag].append(-1);

        const int cols = table->columnCount();
        const QVector<QString> display = buildDisplayRow(tag, row, 1, cols, -1);
        const int r = table->rowCount();
        table->setRowCount(r + 1);
        for (int c = 0; c < cols; ++c)
            table->setItem(r, c, new QTableWidgetItem(display.value(c)));
        statusBar()->showMessage(
            tr("Added a blank first %1 row — edit it, then Save").arg(tag));
        return;
    }

    const int srcIdx = tokenRows.size() - 1;
    const int srcOffset = m_rowOffsets.value(tag).value(srcIdx, 0);

    QStringList cloned = tokenRows.at(srcIdx);

    // Bump the leading numeric identity token (PLU #, DPT code, …) to the
    // next free value.
    const int numIdx = (srcOffset == 1) ? 1 : 0;
    if (numIdx < cloned.size()) {
        bool ok = false;
        const int cur = cloned.at(numIdx).toInt(&ok);
        if (ok) {
            int maxVal = cur;
            for (const QStringList& tw : tokenRows) {
                const int v = tw.value(numIdx).toInt(&ok);
                if (ok && v > maxVal)
                    maxVal = v;
            }
            cloned[numIdx] = QString::number(maxVal + 1);
        }
    }

    // PLU: the name token must contain letters, or the popup/save logic
    // mis-detects the column (name would silently fall back to the code
    // token and typed names would never reach the file).
    int nameIdx = -1;
    if (tag == "PLU") {
        nameIdx = findPluNameIdx(cloned, srcOffset);
        if (nameIdx == -1) {
            const int insertAt = srcOffset + 1;
            cloned.insert(insertAt, tr("NEW PRODUCT"));
            nameIdx = insertAt;
        } else {
            cloned[nameIdx] = tr("NEW PRODUCT");
        }
        // Also clear the cloned product/barcode code when the row has one —
        // leaving it would give the new product the template row's code.
        if (nameIdx > srcOffset + 1)
            cloned[srcOffset + 1] = QString();
    } else if (tag == "BAR") {
        // The barcode data lives in a fixed later field (token[offset+5] in
        // the files we have seen).  Clear it on clone so a newly added BAR
        // row starts blank and the CLI addRowForCli path (or a manual edit)
        // can populate it without the template row's barcode sticking.
        const int barcodeIdx = srcOffset + 5;
        if (barcodeIdx < cloned.size())
            cloned[barcodeIdx] = QString();
    }

    tokenRows.append(cloned);
    m_rowOffsets[tag].append(srcOffset);
    m_rowNameIdx[tag].append(nameIdx);

    const int cols = table->columnCount();
    const QVector<QString> display = buildDisplayRow(tag, cloned, srcOffset,
                                                     cols, nameIdx);
    const int r = table->rowCount();
    table->setRowCount(r + 1);
    for (int c = 0; c < cols; ++c)
        table->setItem(r, c, new QTableWidgetItem(display.value(c)));

    table->scrollToBottom();
    statusBar()->showMessage(
        tr("Added %1 row %2 (created from row %3) — edit it, then Save")
            .arg(tag).arg(r + 1).arg(srcIdx + 1));
}

// ----------------------------------------------------------------
// Issue #4: delete every selected row from the shown table AND from the
// stored token rows, so Save stops writing them (the Save overlay is
// capped at the table's row count, but the block writer emits exactly the
// stored list — the stored list is the one that had to be trimmed).
// ----------------------------------------------------------------
void TmsEditorWindow::removeSelectedRows(const QString& tag)
{
    QTableWidget* table = m_tables.value(tag, nullptr);
    if (!table || !table->selectionModel())
        return;

    const QList<QModelIndex> selected = table->selectionModel()->selectedRows();
    if (selected.isEmpty()) {
        statusBar()->showMessage(tr("Select whole rows to delete (click the row number)"));
        return;
    }
    QVector<int> rows;
    for (const QModelIndex& idx : selected)
        rows.append(idx.row());
    std::sort(rows.begin(), rows.end());

    removeRowsAt(tag, rows);
}

void TmsEditorWindow::removeRowsAt(const QString& tag, const QVector<int>& rows)
{
    QTableWidget* table = m_tables.value(tag, nullptr);
    if (!table)
        return;

    // Remove from the back so the earlier indices stay valid.
    for (int i = rows.size() - 1; i >= 0; --i) {
        const int r = rows.at(i);
        table->removeRow(r);
        if (r >= 0 && r < m_rowTokens[tag].size()) {
            m_rowTokens[tag].remove(r);
            if (r < m_rowOffsets.value(tag).size())
                m_rowOffsets[tag].remove(r);
            if (r < m_rowNameIdx.value(tag).size())
                m_rowNameIdx[tag].remove(r);
        }
    }

    statusBar()->showMessage(
        tr("Deleted %1 row(s) from %2 — Save to write the change")
            .arg(rows.size()).arg(tag));
}

// ----------------------------------------------------------------
// Headless/CLI operations (issue #6). Every mutation goes through the
// exact code path the GUI uses — addRowToSection's clone+numbering, the
// table cells as the single source of truth for Save, the Save overlay
// that maps cells back onto the stored tokens — so a scripted or
// AI-driven edit can never emit a format the GUI itself does not write.
// ----------------------------------------------------------------
bool TmsEditorWindow::listSection(const QString& tag, QStringList* out) const
{
    QTableWidget* table = m_tables.value(tag, nullptr);
    if (!table || !out)
        return false;

    // Header line: "0" is always the identity/code column; the rest follow.
    QStringList header;
    for (int c = 0; c < table->columnCount(); ++c) {
        QTableWidgetItem* it = table->horizontalHeaderItem(c);
        header.append(it ? it->text() : QString::number(c + 1));
    }
    out->append(header.join(QLatin1Char('\t')));

    for (int r = 0; r < table->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < table->columnCount(); ++c) {
            QTableWidgetItem* it = table->item(r, c);
            cells.append(it ? it->text() : QString());
        }
        out->append(cells.join(QLatin1Char('\t')));
    }
    return true;
}

bool TmsEditorWindow::addRowForCli(const QString& tag, const QString& number,
                                   const QString& productCode,
                                   const QString& name, QString* errOut)
{
    QTableWidget* table = m_tables.value(tag, nullptr);
    if (!table) {
        if (errOut)
            *errOut = tr("no such section: %1").arg(tag);
        return false;
    }

    const int rowsBefore = table->rowCount();

    // Exactly the GUI "Add row" path: clone the last row, auto-number it,
    // clear the PLU product code, preset the PLU name. The empty-section
    // bootstrap and its PLU exception come along for free.
    addRowToSection(tag);
    if (table->rowCount() == rowsBefore) {
        if (errOut)
            *errOut = tr("could not add a %1 row").arg(tag);
        return false;
    }
    const int r = table->rowCount() - 1;

    // Overrides, mapped the same way the Save overlay maps display cells
    // onto stored tokens. Empty string = keep the cloned default.
    if (tag == "PLU") {
        if (!number.isEmpty() && table->item(r, 0))
            table->item(r, 0)->setText(number);
        if (!productCode.isEmpty() && table->item(r, 1))
            table->item(r, 1)->setText(productCode);
        if (!name.isEmpty() && table->item(r, 2))
            table->item(r, 2)->setText(name);
    } else if (tag == "BAR") {
        // BAR rows display the row index as "Barcode #" (col 0) and the
        // barcode string as "Barcode data" (col 1).  For a new BAR row the
        // barcode data field starts blank (cleared in addRowToSection), so
        // only an empty name is ignored — a real barcode string is always
        // written.  Map CLI arguments the same way the Save overlay does:
        // number -> col 0 (barcode #), name -> col 1 (barcode data).
        if (!number.isEmpty() && table->item(r, 0))
            table->item(r, 0)->setText(number);
        if (!name.isEmpty() && table->item(r, 1))
            table->item(r, 1)->setText(name);
    } else {
        // Sections: the identity/code column is col 0, value col is col 1.
        if (!productCode.isEmpty() && table->item(r, 0))
            table->item(r, 0)->setText(productCode);
        else if (!number.isEmpty() && table->item(r, 0))
            table->item(r, 0)->setText(number);
        if (!name.isEmpty() && table->item(r, 1))
            table->item(r, 1)->setText(name);
    }
    return true;
}

bool TmsEditorWindow::removeRowsByCode(const QString& tag, const QString& code,
                                       QString* errOut)
{
    QTableWidget* table = m_tables.value(tag, nullptr);
    if (!table) {
        if (errOut)
            *errOut = tr("no such section: %1").arg(tag);
        return false;
    }
    if (code.isEmpty()) {
        if (errOut)
            *errOut = tr("empty code — nothing to match");
        return false;
    }

    QVector<int> rows;
    for (int r = 0; r < table->rowCount(); ++r) {
        // PLU rows match the product code (col 1) first, else the PLU #;
        // other sections match the code column (col 0).
        const int cols = table->columnCount();
        const int prim = (tag == "PLU" && cols > 1) ? 1 : 0;
        const int sec  = (prim == 1) ? 0 : 1;
        QTableWidgetItem* a = (prim < cols) ? table->item(r, prim) : nullptr;
        QTableWidgetItem* b = (sec  < cols) ? table->item(r, sec)  : nullptr;
        if ((a && a->text() == code) || (b && b->text() == code))
            rows.append(r);
    }
    if (rows.isEmpty()) {
        if (errOut)
            *errOut = tr("no %1 row with code '%2'").arg(tag, code);
        return false;
    }

    removeRowsAt(tag, rows);
    return true;
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
    // Write the exact bytes (UTF-8) with NO QIODevice::Text flag — in text
    // mode Windows would translate our already-CRLF terminators to CR CRLF
    // ("\r\r\n"), corrupting the device format.
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }

    const QByteArray bytes = content.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        if (errorOut)
            *errorOut = file.errorString();
        return false;
    }
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
                } else if (tag == "BAR") {
                    // BAR rows store the barcode string in a fixed later field
                    // (token[offset+5] in the files we have seen).  Col 0 of the
                    // table is "Barcode #" (the row index, token[offset]); col 1
                    // is "Barcode data" (token[offset+5]).  Write those two
                    // fields back and leave the hidden numeric fields (tokens
                    // offset+1 .. offset+4) untouched so the device-exact format
                    // survives a round-trip.
                    if (QTableWidgetItem* item = table->item(r, 0)) {
                        const int idx = row.offset;              // Barcode # (row index)
                        if (idx < row.tokens.size())
                            row.tokens[idx] = item->text();
                    }
                    if (QTableWidgetItem* item = table->item(r, 1)) {
                        const int idx = row.offset + 5;         // barcode data
                        if (idx < row.tokens.size())
                            row.tokens[idx] = item->text();
                    }
                } else {
                    // Extend the token row only where the user actually
                    // typed beyond its current span (e.g. a bootstrapped
                    // first row), so untouched short rows stay byte-identical.
                    int needed = row.tokens.size();
                    for (int c = 0; c < table->columnCount(); ++c) {
                        const int idx = row.offset + c;
                        if (QTableWidgetItem* item = table->item(r, c)) {
                            if (idx >= needed && !item->text().isEmpty())
                                needed = idx + 1;
                        }
                    }
                    while (row.tokens.size() < needed)
                        row.tokens.append(QString());
                    for (int c = 0; c < table->columnCount(); ++c) {
                        const int idx = row.offset + c;
                        if (idx >= row.tokens.size())
                            continue;
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

        // Device-exact block emission (matches what the DLX-50 itself
        // writes, byte for byte):
        //   "DWL<TAB>TAG<TAB>" + CRLF on its own line,
        //   one data row per line: fields single-tab joined + trailing
        //   tab + CRLF,
        //   "END<TAB>TAG<TAB>" + CRLF, then one blank line.
        // The trailing tab is what the device leaves on every line,
        // including tabs after an empty last field.
        auto deviceLine = [](const QString& text) {
            QString line = text;
            if (line.isEmpty() || !line.endsWith('\t'))
                line += '\t';
            return line + QStringLiteral("\r\n");
        };

        QString newBlock;
        if (startIdx > 0 && content.at(startIdx - 1) != '\n')
            newBlock += "\r\n";
        newBlock += deviceLine(startMarker);
        for (const SectionRow& row : rows)
            newBlock += deviceLine(row.tokens.join('\t'));
        newBlock += deviceLine(endMarker) + QStringLiteral("\r\n");

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
                    QString("END\tECS\t\r\n"));

    // Exactly one blank line between blocks, and a terminated last line.
    content.replace(QRegularExpression("\n{3,}"), "\n\n");
    if (!content.endsWith('\n'))
        content += '\n';

    return content;
}
