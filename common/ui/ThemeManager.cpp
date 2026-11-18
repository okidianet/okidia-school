// common/ui/ThemeManager.cpp
#include "ThemeManager.h"
#include <QStyleFactory>
#include <QStringBuilder>
#include <QFile>
#include <QProxyStyle>
#include <QPainter>
#include <QStyleOption>

// Tüm checkbox türleri için tek, tutarlı çizim motoru.
// QSS ::indicator kurallarının aksine, bu approach hem QCheckBox hem de
// QTreeView/QTableView/QListView item checkbox'larını kapsar.
class OkidiaStyle : public QProxyStyle {
public:
    explicit OkidiaStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override
    {
        if (element == PE_IndicatorCheckBox ||
            element == PE_IndicatorItemViewItemCheck)
        {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);

            const bool checked  = option->state & State_On;
            const bool partial  = option->state & State_NoChange;
            const bool disabled = !(option->state & State_Enabled);

            QRectF r = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);

            QColor border = disabled
                ? option->palette.mid().color()
                : option->palette.windowText().color();
            QColor bg = (checked || partial)
                ? option->palette.highlight().color()
                : option->palette.base().color();

            painter->setPen(QPen(border, 1.5));
            painter->setBrush(bg);
            painter->drawRoundedRect(r, 2.5, 2.5);

            painter->restore();
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

ThemeManager& ThemeManager::instance() {
    static ThemeManager instance;
    return instance;
}

ThemeManager::ThemeManager(QObject* parent) : QObject(parent) {
    // Hazır 6 Tema
    m_themes = {
        {tr("Gece Mavisi (Koyu)"), ThemeMode::Dark, QColor("#1e88e5")},   // Blue
        {tr("Orman Yeşili (Koyu)"), ThemeMode::Dark, QColor("#43a047")},  // Green
        {tr("Yüksek Kontrast (Koyu)"), ThemeMode::Dark, QColor("#ffffff")}, // High Contrast B&W
        {tr("Günışığı Mavisi (Açık)"), ThemeMode::Light, QColor("#1976d2")}, // Blue
        {tr("Açık Turuncu (Açık)"), ThemeMode::Light, QColor("#f57c00")},    // Orange
        {tr("Sepya (Açık)"), ThemeMode::Light, QColor("#8b5a2b")}         // Sepia
    };
}

void ThemeManager::initialize(QApplication* app) {
    m_app = app;
    m_app->setStyle(new OkidiaStyle());
}

QList<ThemeManager::ThemeDef> ThemeManager::availableThemes() const {
    return m_themes;
}

void ThemeManager::applyTheme(int index) {
    if (index >= 0 && index < m_themes.size()) {
        const ThemeDef& t = m_themes[index];
        applyTheme(t.mode, t.primaryColor);
    } else {
        // Varsayılan
        applyTheme(ThemeMode::Dark, QColor("#1e88e5"));
    }
}

QColor ThemeManager::calculateContrastText(const QColor& background) const {
    // Relative luminance / YIQ formülü: (R*299 + G*587 + B*114) / 1000
    int yiq = (background.red() * 299 + background.green() * 587 + background.blue() * 114) / 1000;
    return (yiq >= 128) ? QColor("#000000") : QColor("#ffffff");
}

void ThemeManager::applyTheme(ThemeMode mode, const QColor& primaryColor) {
    if (!m_app) return;

    m_currentMode = mode;
    QPalette p = m_app->palette();

    // Renk karıştırma (tint) yardımcı fonksiyonu
    auto tint = [](const QColor& base, const QColor& tintColor, int alpha) -> QColor {
        int r = (base.red() * (255 - alpha) + tintColor.red() * alpha) / 255;
        int g = (base.green() * (255 - alpha) + tintColor.green() * alpha) / 255;
        int b = (base.blue() * (255 - alpha) + tintColor.blue() * alpha) / 255;
        return QColor(r, g, b);
    };

    QColor window, windowText, base, alternateBase, text, button, buttonText;
    QColor light, midlight, dark, mid, shadow;
    QColor disabledText = QColor(128, 128, 128);

    QColor successColor, errorColor, warningColor, offlineBg, onlineBg;

    if (primaryColor == QColor("#8b5a2b")) {
        // Sepya Özel Tonlaması (Eski kağıt / sarımsı)
        window = QColor("#f4ecd8");
        base = QColor("#fdf6e3"); // Solarized light yellow tarzı
        alternateBase = QColor("#f6eedb");
        button = QColor("#e9dec2");
        
        windowText = QColor("#4a3b32");
        text = QColor("#3e322a");
        buttonText = QColor("#4a3b32");

        light = QColor("#ffffff");
        midlight = QColor("#f4ecd8");
        dark = QColor("#c2b395");
        mid = QColor("#d6c7ab");
        shadow = QColor("#8b7c62");

        successColor = QColor("#438c4b");
        errorColor = QColor("#c62828");
        warningColor = QColor("#e67e22");
        offlineBg = errorColor;
        onlineBg = successColor;
    } else if (primaryColor == QColor("#ffffff")) {
        // Yüksek Kontrast (Siyah/Beyaz)
        window = QColor("#000000");
        base = QColor("#0a0a0a");
        alternateBase = QColor("#141414");
        button = QColor("#222222");
        
        windowText = QColor("#ffffff");
        text = QColor("#ffffff");
        buttonText = QColor("#ffffff");
        
        // 3D element renkleri yüksek kontrastlı
        light = QColor("#ffffff");
        midlight = QColor("#dddddd");
        dark = QColor("#aaaaaa");
        mid = QColor("#888888");
        shadow = QColor("#000000");

        successColor = QColor("#00ff00");
        errorColor = QColor("#ff0000");
        warningColor = QColor("#ffff00");
        offlineBg = errorColor;
        onlineBg = successColor;
    } else if (mode == ThemeMode::Dark) {
        // Ana zemin renklerini tema rengiyle (primaryColor) %8 oranında karıştır (tint)
        window = tint(QColor(32, 32, 32), primaryColor, 20);
        base = tint(QColor(48, 48, 48), primaryColor, 15);
        alternateBase = tint(QColor(56, 56, 56), primaryColor, 15);
        button = tint(QColor(64, 64, 64), primaryColor, 25);
        
        windowText = QColor(240, 240, 240);
        text = QColor(240, 240, 240);
        buttonText = QColor(240, 240, 240);
        
        // Checkbox çerçevesi = yazı rengi (windowText)
        light = tint(QColor(220, 220, 220), primaryColor, 10);
        midlight = tint(QColor(200, 200, 200), primaryColor, 10);
        dark = windowText;  // Koyu temada checkbox çerçevesi çıkık açık renk
        mid = tint(QColor(195, 195, 195), primaryColor, 10);
        shadow = QColor(10, 10, 10);

        successColor = QColor("#a6e3a1");
        errorColor = QColor("#f38ba8");
        warningColor = QColor("#f9e2af");
        
        // Çevrim dışı ekran arka planı için çok parlak bir kırmızı yerine, 
        // daha koyu bir kırmızı kullanalım ki üzerindeki yazı BEYAZ (açık renk) olsun.
        offlineBg = QColor("#b71c1c"); // Material Red 900
        onlineBg = successColor;
    } else {
        // Ana zemin renklerini tema rengiyle (primaryColor) %5 oranında karıştır
        window = tint(QColor(240, 240, 240), primaryColor, 12);
        base = tint(QColor(255, 255, 255), primaryColor, 8);
        alternateBase = tint(QColor(245, 245, 245), primaryColor, 10);
        button = tint(QColor(220, 220, 220), primaryColor, 15);
        
        windowText = QColor(30, 30, 30);
        text = QColor(30, 30, 30);
        buttonText = QColor(30, 30, 30);

        light = QColor(255, 255, 255);
        midlight = tint(QColor(175, 175, 175), primaryColor, 12);
        dark = windowText;  // Açık temada checkbox çerçevesi koyu yazı rengi
        mid = tint(QColor(75, 75, 75), primaryColor, 12);
        shadow = QColor(105, 105, 105);

        successColor = QColor("#2e7d32");
        errorColor = QColor("#c62828");
        warningColor = QColor("#f57f17");
        offlineBg = errorColor;
        onlineBg = successColor;
    }

    m_successColor = successColor;
    m_errorColor   = errorColor;
    m_warningColor = warningColor;
    m_onlineBg     = onlineBg;
    m_offlineBg    = offlineBg;

    QColor highlight = primaryColor;
    QColor highlightedText = calculateContrastText(highlight);

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, windowText);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alternateBase);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, windowText);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, buttonText);
    p.setColor(QPalette::BrightText, errorColor);
    p.setColor(QPalette::Link, primaryColor);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, highlightedText);
    
    p.setColor(QPalette::Light, light);
    p.setColor(QPalette::Midlight, midlight);
    p.setColor(QPalette::Dark, dark);
    p.setColor(QPalette::Mid, mid);
    p.setColor(QPalette::Shadow, shadow);

    p.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);

    // Global Stylesheet ile UI bileşenlerini sabitlemek
    QString qss = QString(
        "QMainWindow { background-color: palette(window); }"
        "QDialog { background-color: palette(window); }"
        "QWidget { color: palette(window-text); }"
        "QLabel { color: palette(window-text); }"
        "QMenu { background-color: palette(window); color: palette(window-text); border: 1px solid palette(button); border-radius: 6px; padding: 4px; font-size: 13px; font-weight: normal; }"
        "QMenu::item { background-color: transparent; padding: 6px 30px 6px 15px; border-radius: 4px; font-size: 13px; font-weight: normal; }"
        "QMenu::item:selected { background-color: palette(highlight); color: palette(highlighted-text); }"
        "QMenu::item:disabled { color: palette(disabled); }"
        "QMenu::separator { height: 1px; background-color: palette(button); margin: 4px 8px; }"
        "QTabWidget::pane { border: 1px solid palette(button); border-radius: 4px; background: palette(base); }"
        "QTabBar::tab { background: palette(button); color: palette(button-text); padding: 8px 16px; border: 1px solid palette(mid); border-bottom: none; border-top-left-radius: 4px; border-top-right-radius: 4px; margin-right: 2px; font-weight: bold; }"
        "QTabBar::tab:selected { background: palette(base); border-color: palette(button); border-bottom-color: palette(base); color: palette(highlight); }"
        "QTabBar::tab:hover:!selected { background: palette(highlight); color: palette(highlighted-text); }"
        "QTableView, QTreeWidget, QListWidget { background-color: palette(base); alternate-background-color: palette(alternate-base); border: 1px solid palette(button); }"
        "QTreeView::item { color: palette(text); }"
        "QHeaderView::section { background-color: palette(button); color: palette(button-text); padding: 4px; border: 1px solid palette(window); }"
        "QPushButton { background-color: palette(button); color: palette(button-text); border: 1px solid palette(mid); border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }"
        "QPushButton:pressed { background-color: %1; }"
        "QPushButton:checked { background-color: palette(highlight); color: palette(highlighted-text); border-color: palette(highlight); }"
        "QPushButton:disabled { background-color: palette(window); color: palette(disabled); }"
        "QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox { background-color: palette(base); color: palette(text); border: 1px solid palette(button); border-radius: 4px; padding: 4px 6px; }"
        "QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid palette(highlight); }"
        "QScrollBar:vertical { background: palette(base); width: 14px; margin: 0px 0px 0px 0px; }"
        "QScrollBar::handle:vertical { background: palette(button); min-height: 20px; border-radius: 7px; margin: 2px; }"
        "QScrollBar::handle:vertical:hover { background: palette(highlight); }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar:horizontal { background: palette(base); height: 14px; margin: 0px 0px 0px 0px; }"
        "QScrollBar::handle:horizontal { background: palette(button); min-width: 20px; border-radius: 7px; margin: 2px; }"
        "QScrollBar::handle:horizontal:hover { background: palette(highlight); }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
        "QLabel[class=\"headerLabel\"] { font-size: 13px; font-weight: bold; border-bottom: 1px solid palette(button); }"
        "QLabel[status=\"error\"] { color: %2; font-weight: bold; }"
        "QLabel[status=\"warning\"] { color: %3; font-weight: bold; }"
        "QLabel[status=\"success\"] { color: %4; font-weight: bold; }"
        "QLabel[status=\"info\"] { color: palette(disabled); }"
        "QLabel[class=\"statusIconOffline\"] { background-color: %5; border-radius: 6px; }"
        "QLabel[class=\"statusIconOnline\"] { background-color: %6; border-radius: 6px; }"
        "QLabel[class=\"screenLabelOffline\"] { background-color: %5; border-radius: 4px; color: %7; font-size: 14px; font-weight: bold; }"
        "QLabel[class=\"screenLabelOnline\"] { background-color: palette(base); border-radius: 4px; color: palette(mid); font-size: 13px; font-weight: bold; }"
        "QPushButton[class=\"primary\"] { font-weight: bold; border: 1px solid palette(highlight); }"
        "QPushButton[class=\"link\"] { background-color: transparent; border: none; color: palette(link); text-decoration: underline; padding: 2px; }"
        "QPushButton[class=\"link\"]:hover { color: palette(highlight); }"
        "QLabel[class=\"recoveryKey\"] { font-family: monospace; font-size: 20px; font-weight: bold; color: %2; border: 2px dashed %2; padding: 15px; background: palette(base); }"
        "ThumbnailGroupFrame { border: 0.5px solid palette(mid); border-radius: 8px; margin: 0 4px 4px 4px; }"
        "QLabel[class=\"groupHeader\"] { border-bottom: 0.5px solid palette(mid); border-radius: 8px 8px 0 0; font-size: 12px; }"
        "QLabel[class=\"groupHeader\"]:hover { background: palette(highlight); color: palette(highlighted-text); }"
        "QLabel[class=\"groupHeader\"][isUnassigned=\"true\"] { background: palette(base); color: palette(mid); }"
        "QLabel[class=\"groupHeader\"][isUnassigned=\"false\"] { background: palette(button); color: palette(button-text); }"
        "QTreeView::item, QTableView::item, QListView::item { min-height: 20px; padding: 2px 4px; }"
    ).arg(highlight.darker(120).name())
     .arg(errorColor.name())
     .arg(warningColor.name())
     .arg(successColor.name())
     .arg(offlineBg.name())
     .arg(onlineBg.name())
     .arg(calculateContrastText(offlineBg).name());

    m_app->setPalette(p);          // 1) stylesheet öncesi: ilk yüklemede re-polish palette'i görsün
    m_app->setStyleSheet(qss);
    m_app->setPalette(p);          // 2) stylesheet sonrası: olası palette reset'i önle

    emit themeChanged();
}


#include <QIconEngine>

class ThemeIconEngine : public QIconEngine {
public:
    ThemeIconEngine(const QString& path) : m_basePath(path) {}

    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        getActualIcon().paint(painter, rect, Qt::AlignCenter, mode, state);
    }

    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        return getActualIcon().pixmap(size, mode, state);
    }

    QIconEngine *clone() const override {
        return new ThemeIconEngine(m_basePath);
    }

private:
    QIcon getActualIcon() const {
        QString actualPath = m_basePath;
        if (ThemeManager::instance().currentMode() == ThemeManager::ThemeMode::Dark) {
            if (actualPath.startsWith(":/ui/")) {
                actualPath.replace(":/ui/", ":/ui_dark/");
            }
        }
        return QIcon(actualPath);
    }
    QString m_basePath;
};

QIcon ThemeManager::icon(const QString& path) {
    if (path.startsWith(":/ui/")) {
        return QIcon(new ThemeIconEngine(path));
    }
    return QIcon(path);
}
