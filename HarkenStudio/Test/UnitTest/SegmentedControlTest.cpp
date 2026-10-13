#include "Core/QtControlSkin.h"
#include "Widgets/Common/SegmentedControl.h"
#include "Support/AITestSupport.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QToolButton>

namespace {

class SegmentedControlTest : public testing::Test
{
protected:
    void SetUp() override
    {
        window_.setStyleSheet(VNX::QtControlSkin::current().applicationStyleSheet());
        auto* row = new QHBoxLayout(&window_);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(0);
        control_ = new VNX::SegmentedControl(&window_);
        control_->setAdaptiveSizing(true);
        row->addWidget(control_);
        row->addStretch(1);
    }

    void resizeWindow(int width)
    {
        window_.resize(width, 80);
        window_.show();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QApplication::processEvents();
        EXPECT_EQ(window_.width(), width);
    }

    void expectAdaptiveWidths()
    {
        ASSERT_TRUE(VNX::TestSupport::spinUntil([this]() {
            const int naturalWidth = control_->sizeHint().width();
            const int available = qMin(naturalWidth, window_.width());
            if (control_->width() != available) return false;
            for (int i = 0; auto* button = control_->button(i); ++i) {
                const double expected = double(available) * button->sizeHint().width() / naturalWidth;
                if (qAbs(button->width() - expected) > 1) return false;
            }
            return true;
        }));
        int right = 0;
        for (int i = 0; auto* button = control_->button(i); ++i) {
            EXPECT_TRUE(button->isVisible());
            EXPECT_GT(button->width(), 0);
            EXPECT_EQ(button->x(), right);
            right += button->width();
            EXPECT_TRUE(control_->rect().contains(button->geometry()));
        }
        EXPECT_EQ(right, control_->width());
    }

    QWidget window_;
    VNX::SegmentedControl* control_ = nullptr;
};

TEST_F(SegmentedControlTest, NaturalWidthsCompressProportionallyAndRemainClickable)
{
    const QList<QPair<QString, QVariant>> options{
        {QStringLiteral("PC"), 1},
        {QStringLiteral("Android_Medium"), 2},
        {QStringLiteral("Android_High_Quality_LongName"), 3}};
    control_->setOptions(options);
    resizeWindow(1600);
    expectAdaptiveWidths();
    const int naturalWidth = control_->sizeHint().width();
    EXPECT_LT(control_->button(0)->width(), control_->button(1)->width());
    EXPECT_LT(control_->button(1)->width(), control_->button(2)->width());
    for (int width : {naturalWidth + 80, naturalWidth, naturalWidth * 2 / 3, naturalWidth / 3,
                      naturalWidth * 2 / 3, naturalWidth + 160}) {
        SCOPED_TRACE(width);
        resizeWindow(width);
        expectAdaptiveWidths();
        for (int i = 0; i < options.size(); ++i) {
            auto* button = control_->button(i);
            const QPoint center = button->rect().center();
            const QPoint global = button->mapToGlobal(center);
            QMouseEvent press(QEvent::MouseButtonPress, center, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(button, &press);
            QMouseEvent release(QEvent::MouseButtonRelease, center, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(button, &release);
            EXPECT_TRUE(button->isChecked());
            EXPECT_EQ(button->text(), options[i].first);
            EXPECT_EQ(button->toolTip(), options[i].first);
            EXPECT_EQ(button->property("segmentValue"), options[i].second);
        }
        EXPECT_EQ(control_->sizeHint().width(), naturalWidth);
    }
}

TEST_F(SegmentedControlTest, ModeOptionsAndFontChangesRecomputeTheLayout)
{
    delete window_.layout()->takeAt(1);
    control_->setAdaptiveSizing(false);
    control_->setOptions({{QStringLiteral("Seconds"), false}, {QStringLiteral("Frames"), true}});
    resizeWindow(1000);
    EXPECT_EQ(control_->width(), window_.width());
    control_->setAdaptiveSizing(true);
    resizeWindow(1000);
    expectAdaptiveWidths();
    const int originalWidth = control_->width();
    control_->setOptions({{QStringLiteral("Short"), 1}, {QStringLiteral("Much_Longer_Option_Name"), 2},
                         {QStringLiteral("Third_Option"), 3}});
    resizeWindow(1000);
    expectAdaptiveWidths();
    EXPECT_GT(control_->width(), originalWidth);
    const int beforeFontChange = control_->width();
    window_.setStyleSheet(window_.styleSheet()
        + QStringLiteral("\nQToolButton[segmentedControl=\"true\"] { font-size: 20pt; }\n"));
    resizeWindow(2000);
    expectAdaptiveWidths();
    EXPECT_GT(control_->width(), beforeFontChange);
    resizeWindow(control_->sizeHint().width() / 2);
    expectAdaptiveWidths();
    control_->setAdaptiveSizing(false);
    resizeWindow(2000);
    EXPECT_EQ(control_->width(), window_.width());
}

}
