#pragma once

#include <QStringList>
#include <QVector>
#include <QWidget>

#include "../providers/diskprovider.h"

class QGridLayout;
class QGroupBox;
class QLabel;
class HistoryGraph;
class Sampler;

// One resource page per block device: a throughput chart, an active-time
// chart, the hardware description, and the filesystems living on it.
class DiskPage : public QWidget
{
    Q_OBJECT

public:
    DiskPage(Sampler *sampler, const DiskDeviceInfo &device,
             QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyTheme();
    QWidget *buildDetailsBox();
    QWidget *buildFilesystemsBox();
    void addDetailRow(QGridLayout *grid, int &row, const QString &key,
                      const QString &value, QLabel **out = nullptr);
    void updateFilesystems(const QVector<DiskMountInfo> &mounts);
    void onDiskSample(const QVector<DiskSnapshot> &snapshots);

    const DiskDeviceInfo m_device;

    QLabel *m_summary;
    QLabel *m_detail;
    QLabel *m_readValue = nullptr;
    QLabel *m_writeValue = nullptr;
    QLabel *m_queueValue = nullptr;
    QVector<QLabel *> m_secondaryLabels;

    HistoryGraph *m_throughputGraph = nullptr;
    HistoryGraph *m_activeGraph = nullptr;
    int m_readSeries = -1;
    int m_writeSeries = -1;
    int m_activeSeries = -1;

    QGroupBox *m_filesystemsBox = nullptr;
    QGridLayout *m_filesystemsGrid = nullptr;
    // Rebuilding the rows every tick would fight text selection, so they are
    // only rebuilt when the set of mount points actually changes.
    QStringList m_filesystemKeys;
    QVector<QLabel *> m_filesystemValues;

    static constexpr int kHistorySeconds = 60;
};
