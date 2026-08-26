#pragma once

#include <QVector>
#include <QWidget>

#include "../providers/gpuprovider.h"

class QLabel;
class HistoryGraph;
class Sampler;

// One resource page per GPU: utilization and video-engine history, a VRAM
// chart where the driver reports memory, and a details block.
//
// A card whose metrics nothing can read (an NVIDIA card with nvidia-utils
// missing) still gets a page, showing what is known about it plus the reason
// the graphs are absent. Silently dropping the row would leave a user
// hunting for a GPU the app can plainly see.
class GpuPage : public QWidget
{
    Q_OBJECT

public:
    GpuPage(Sampler *sampler, const GpuDeviceInfo &device,
            QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyTheme();
    QWidget *buildUnavailableBody();
    QWidget *buildDetailsBox();
    void addDetailRow(class QGridLayout *grid, int &row, const QString &key,
                      const QString &value, QLabel **out = nullptr);
    void onGpuSample(const QVector<GpuSnapshot> &snapshots);

    const GpuDeviceInfo m_device;

    QLabel *m_summary;
    QLabel *m_detail = nullptr;
    QLabel *m_memoryDetail = nullptr;
    QLabel *m_temperatureValue = nullptr;
    QLabel *m_powerValue = nullptr;
    QLabel *m_clockValue = nullptr;
    QVector<QLabel *> m_secondaryLabels;

    HistoryGraph *m_utilizationGraph = nullptr;
    HistoryGraph *m_memoryGraph = nullptr;
    // Bytes per unit on the memory chart's Y axis. An integrated GPU's VRAM
    // carve-out is a few hundred MiB, which plotted in GiB is an unreadable
    // sliver against a 0.5-high axis.
    double m_memoryDivisor = 1073741824.0;
    int m_busySeries = -1;
    int m_videoSeries = -1;
    int m_vramSeries = -1;

    static constexpr int kHistorySeconds = 60;
};
