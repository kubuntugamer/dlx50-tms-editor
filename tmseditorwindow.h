#pragma once

#include <QMainWindow>
#include <QString>
#include <QMap>
#include <QVector>
#include <QStringList>

class QTabWidget;
class QTableWidget;
class QMenu;
class QAction;
class QMenuBar;
class QStatusBar;
class Updater;

class TmsEditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit TmsEditorWindow(const QString& filePath = QString());
    ~TmsEditorWindow();

public slots:
    void openFile();
    void saveFile();     // DLX‑50 style: back up to <file>.BAK, overwrite in place
    void saveFileAs();   // Save to a path chosen by the user

    // Add/remove products and rows (issue #4): “Add row” clones the last
    // row of the section as a numbered draft (PLU: next free number, name
    // replaced with “NEW PRODUCT”); “Delete selected” removes every
    // selected row from both the table and the stored token rows so Save
    // stops writing them. Public slots so test harnesses can drive them.
    void addRowToSection(const QString& tag);
    void removeSelectedRows(const QString& tag);

    // Headless/CLI operations (issue #6): the same load/clone/overlay/save
    // machinery the GUI buttons use, so a scripted or AI-driven edit can
    // never emit a format the GUI itself would write. No dialogs, no
    // statusBar interplay beyond harmless messages.
    bool fileLoaded() const { return m_fileLoaded; }

    // Display rows of a section as tab-joined lines, header line first.
    bool listSection(const QString& tag, QStringList* out) const;

    // Add a row like the "Add row" button (clone the last row, auto-number
    // the leading identity token), then override values the caller supplied:
    //   PLU      : number→col 0 (PLU #), productCode→col 1, name→col 2 (name)
    //   sections : productCode→col 0 (code), name→col 1 (value/description)
    // Pass empty strings to keep the defaults from the clone step.
    bool addRowForCli(const QString& tag, const QString& number,
                      const QString& productCode, const QString& name,
                      QString* errOut);

    // Delete the row(s) whose product code (PLU) or code column matches.
    bool removeRowsByCode(const QString& tag, const QString& code,
                          QString* errOut);

private slots:
    void onCellChanged();

private:
    void buildUI();

    void startUpdateCheck();

    bool loadFile(const QString& path);

    bool m_fileLoaded = false;

    void removeRowsAt(const QString& tag, const QVector<int>& rows);

    QString rawSection(const QString& tag);

    void populateTab(const QString& tag, QTableWidget* table);

    // Self-update (issue #3): checks GitHub Releases, downloads, installs
    // (Linux: pkexec prompt; Windows: exit-and-relaunch helper).
    Updater* m_updater = nullptr;

    QString m_currentFilePath;
    QString m_rawContent;

    QMap<QString, int> m_colCount;
    QTabWidget* m_tabWidget = nullptr;
    QMap<QString, QTableWidget*> m_tables;

    // Full tab-separated token list of every loaded data row, per section,
    // with the leading section tag removed. Save writes these back so the
    // fields that have no column of their own are never lost.
    QMap<QString, QVector<QStringList>> m_rowTokens;

    // Per row: how many leading tokens are the section tag (0 or 1), and the
    // index of the product-name token inside m_rowTokens (PLU only, -1 when
    // the row has no name token).
    QMap<QString, QVector<int>> m_rowOffsets;
    QMap<QString, QVector<int>> m_rowNameIdx;

    // Rebuild the full file content from the currently displayed tables.
    // Lines that are not part of any DWL…END section are kept verbatim;
    // each DWL…END section is reconstructed from its table.
    QString rebuildFullContentFromTables();
};
