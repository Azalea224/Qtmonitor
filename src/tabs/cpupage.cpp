#include "cpupage.h"

#include <QEvent>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include "../providers/hardwareinfo.h"
#include "../sampler.h"
#include "minigraph.h"
#include "theming.h"

CpuPage::CpuPage(Sampler *sampler, QWidget *parent)
    : QWidget(parent)
{
    // Content sits in a scroll area so the details box is never clipped
    // when the window is short.
    auto *content = new QWidget;
    auto *layout = new QGridLayout(content);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto summaryFont = font();
    summaryFont.setPointSize(summaryFont.pointSize() + 4);
    summaryFont.setBold(true);

    m_summary = new QLabel(tr("CPU"), content);
    m_summary->setFont(summaryFont);
    m_detail = new QLabel(content);
    m_secondaryLabels.append(m_detail);
    theming::markSecondary(m_detail, palette());

    layout->addWidget(m_summary, 0, 0);
    layout->addWidget(m_detail, 0, 1, Qt::AlignRight);

    auto *coreBox = new QGroupBox(tr("Per-core utilization (%)"), content);
    m_coreGrid = new QGridLayout(coreBox);
    m_coreGrid->setSpacing(6);
    coreBox->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    layout->addWidget(coreBox, 1, 0, 1, 2);
    layout->addWidget(buildDetailsBox(), 2, 0, 1, 2);
    // Spare vertical space goes to an empty bottom row, not the header
    layout->setRowStretch(3, 1);

    auto *scroll = new QScrollArea(this);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->addWidget(scroll);

    connect(sampler, &Sampler::cpuSampled, this, &CpuPage::onCpuSample);
}

void CpuPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange) {
        for (QLabel *label : m_secondaryLabels) {
            theming::markSecondary(label, palette());
        }
    }
    QWidget::changeEvent(event);
}

void CpuPage::buildCoreGrid(int coreCount)
{
    if (coreCount <= 0 || !m_coreGraphs.isEmpty()) {
        return;
    }
    // 8 columns keeps cells readable at default window size on wide-core CPUs
    const int columns = coreCount > 16 ? 8 : 4;
    for (int i = 0; i < coreCount; ++i) {
        auto *cell = new QWidget(this);
        auto *cellLayout = new QGridLayout(cell);
        cellLayout->setContentsMargins(4, 2, 4, 2);
        cellLayout->setSpacing(0);

        auto *name = new QLabel(QString::number(i), cell);
        auto font = name->font();
        font.setPointSize(qMax(7, font.pointSize() - 2));
        name->setFont(font);
        m_secondaryLabels.append(name);
        theming::markSecondary(name, palette());

        auto *graph = new MiniGraph(kHistorySeconds, cell);
        cellLayout->addWidget(name, 0, 0);
        cellLayout->addWidget(graph, 1, 0);

        m_coreGrid->addWidget(cell, i / columns, i % columns);
        m_coreGraphs.append(graph);
    }
}

QWidget *CpuPage::buildDetailsBox()
{
    const CpuStaticInfo info = loadCpuStaticInfo();

    auto *box = new QGroupBox(tr("Details"), this);
    // QGridLayout, not QFormLayout: QFormLayout clips word-wrapped labels
    // vertically (broken height-for-width), which truncated long values.
    auto *grid = new QGridLayout(box);
    grid->setContentsMargins(12, 8, 12, 8);
    grid->setColumnStretch(1, 1);

    int row = 0;
    // Every value is a single short line — no word wrap anywhere, since
    // wrapped QLabels get vertically clipped inside layouts.
    auto addRow = [this, box, grid, &row](const QString &key, const QString &value,
                                          QLabel **out = nullptr) {
        auto *keyLabel = new QLabel(key.isEmpty() ? QString() : key + QLatin1Char(':'), box);
        auto *valueLabel = new QLabel(value, box);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_secondaryLabels.append(valueLabel);
        theming::markSecondary(valueLabel, palette());
        grid->addWidget(keyLabel, row, 0, Qt::AlignLeft | Qt::AlignTop);
        grid->addWidget(valueLabel, row, 1, Qt::AlignLeft | Qt::AlignTop);
        ++row;
        if (out) {
            *out = valueLabel;
        }
    };

    addRow(tr("Model"), info.modelName);
    if (info.maxFreqGhz > 0.0) {
        addRow(tr("Max speed"),
               tr("%1 GHz").arg(info.maxFreqGhz, 0, 'f', 2));
    }
    addRow(tr("Driver"), info.driver);
    addRow(tr("Governor"), info.governor, &m_governorValue);
    addRow(tr("Power preference"), info.energyPreference, &m_powerPrefValue);

    // One row per cache level, keyed "Cache (L1d)" etc.
    for (const auto &cache : info.caches) {
        addRow(tr("Cache (%1)").arg(cache.first), cache.second);
    }

    // Instruction sets chunked across rows so each label stays one line
    constexpr int kPerRow = 5;
    for (int i = 0; i < info.instructionSets.size(); i += kPerRow) {
        const QStringList chunk = info.instructionSets.mid(i, kPerRow);
        addRow(i == 0 ? tr("Instruction sets") : QString(),
               chunk.join(QStringLiteral(" · ")));
    }

    return box;
}

void CpuPage::onCpuSample(const CpuSnapshot &snapshot)
{
    if (m_coreGraphs.isEmpty()) {
        buildCoreGrid(snapshot.coreCount);
    }

    for (int i = 0; i < snapshot.perCorePercents.size() && i < m_coreGraphs.size(); ++i) {
        m_coreGraphs.at(i)->pushValue(snapshot.perCorePercents.at(i));
    }

    m_summary->setText(tr("CPU — %1% overall")
                           .arg(snapshot.totalPercent, 0, 'f', 1));
    m_detail->setText(tr("%1 logical CPUs · %2 GHz")
                          .arg(snapshot.coreCount)
                          .arg(snapshot.currentFreqGhz, 0, 'f', 2));

    // Governor / power preference can change at runtime — refresh each tick
    const CpuDynamicInfo dynamic = loadCpuDynamicInfo();
    if (m_governorValue && !dynamic.governor.isEmpty()) {
        m_governorValue->setText(dynamic.governor);
    }
    if (m_powerPrefValue) {
        m_powerPrefValue->setText(dynamic.energyPreference);
        m_powerPrefValue->setVisible(!dynamic.energyPreference.isEmpty());
    }
}
