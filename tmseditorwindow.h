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

private slots:
    void onCellChanged();

private:
    void buildUI();

    bool loadFile(const QString& path);

    QString rawSection(const QString& tag);

    void populateTab(const QString& tag, QTableWidget* table);

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
