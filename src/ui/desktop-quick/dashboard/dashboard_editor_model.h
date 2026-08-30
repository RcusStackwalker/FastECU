#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>

#include <cstddef>
#include <optional>
#include <vector>

#include "src/backend/dashboard/dashboard_document.h"
#include "src/ui/desktop-quick/dashboard/dashboard_card_model.h"
#include "src/ui/desktop-quick/dashboard/dashboard_document_controller.h"

namespace fastecu::desktop_quick
{

class DashboardEditorModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(QString selectedCardId READ selectedCardId WRITE selectCard NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList channelChoices READ channelChoices NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList conversionChoices READ conversionChoices NOTIFY choicesChanged)
    Q_PROPERTY(QVariantList displayTypeChoices READ displayTypeChoices CONSTANT)
    Q_PROPERTY(QString selectedChannelId READ selectedChannelId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedConversionId READ selectedConversionId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedTitle READ selectedTitle NOTIFY selectionChanged)
    Q_PROPERTY(CardDisplayType selectedDisplayType READ selectedDisplayType NOTIFY selectionChanged)
    Q_PROPERTY(double selectedGaugeMinimum READ selectedGaugeMinimum NOTIFY selectionChanged)
    Q_PROPERTY(double selectedGaugeMaximum READ selectedGaugeMaximum NOTIFY selectionChanged)
    Q_PROPERTY(double selectedGaugeStep READ selectedGaugeStep NOTIFY selectionChanged)
    Q_PROPERTY(int selectedSparklineHistorySeconds READ selectedSparklineHistorySeconds NOTIFY selectionChanged)
    Q_PROPERTY(bool canAdd READ canAdd NOTIFY availabilityChanged)
    Q_PROPERTY(bool canRemove READ canRemove NOTIFY availabilityChanged)
    Q_PROPERTY(bool canMoveUp READ canMoveUp NOTIFY availabilityChanged)
    Q_PROPERTY(bool canMoveDown READ canMoveDown NOTIFY availabilityChanged)

  public:
    enum Role
    {
        CardIdRole = Qt::UserRole + 1,
        TitleRole,
        ChannelNameRole,
        ConversionIdRole,
        DisplayTypeRole,
    };
    Q_ENUM(Role)

    explicit DashboardEditorModel(DashboardDocumentController& controller, QObject *parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString selectedCardId() const;
    QVariantList channelChoices() const;
    QVariantList conversionChoices() const;
    QVariantList displayTypeChoices() const;
    QString selectedChannelId() const;
    QString selectedConversionId() const;
    QString selectedTitle() const;
    CardDisplayType selectedDisplayType() const;
    double selectedGaugeMinimum() const;
    double selectedGaugeMaximum() const;
    double selectedGaugeStep() const;
    int selectedSparklineHistorySeconds() const;
    bool canAdd() const;
    bool canRemove() const;
    bool canMoveUp() const;
    bool canMoveDown() const;

    Q_INVOKABLE void selectCard(const QString& card_id);
    Q_INVOKABLE void addCard(const QString& channel_id, const QString& conversion_id);
    Q_INVOKABLE void removeSelected();
    Q_INVOKABLE void moveSelectedUp();
    Q_INVOKABLE void moveSelectedDown();
    Q_INVOKABLE void setSelectedChannel(const QString& channel_id);
    Q_INVOKABLE void setSelectedConversion(const QString& conversion_id);
    Q_INVOKABLE void setSelectedTitle(const QString& title);
    Q_INVOKABLE void setSelectedDisplayType(CardDisplayType display_type);
    Q_INVOKABLE void setSelectedGaugeBounds(double minimum, double maximum, double step);
    Q_INVOKABLE void setSelectedSparklineHistorySeconds(int seconds);

  signals:
    void selectionChanged();
    void choicesChanged();
    void availabilityChanged();

  private:
    const dashboard::DashboardDocument *document() const;
    const dashboard::DashboardCard *selectedCard() const;
    const dashboard::DashboardChannel *channelFor(const dashboard::DashboardCard& card) const;
    const dashboard::DashboardConversion *conversionFor(const dashboard::DashboardCard& card) const;
    std::vector<std::size_t> orderedCardIndexes() const;
    static void orderCards(std::vector<dashboard::DashboardCard>& cards);
    static void renumberCards(std::vector<dashboard::DashboardCard>& cards);
    static dashboard::DashboardCard *selectedCard(dashboard::DashboardDocument& document,
                                                  const QString& selected_card_id);
    void commit(dashboard::DashboardDocument candidate, const QString& selected_card_id);
    void resetFromCommittedDocument();
    void refreshProperties();

    DashboardDocumentController& controller_;
};

} // namespace fastecu::desktop_quick
