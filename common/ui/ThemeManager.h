// common/ui/ThemeManager.h
#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QColor>
#include <QPalette>
#include <QApplication>
#include <QList>
#include <QString>

class ThemeManager : public QObject {
    Q_OBJECT
public:
    enum class ThemeMode {
        Dark,
        Light
    };

    struct ThemeDef {
        QString name;
        ThemeMode mode;
        QColor primaryColor;
    };

    static ThemeManager& instance();

    // Uygulama başlatılırken çağrılmalı
    void initialize(QApplication* app);

    // Tanımlı hazır temalar
    QList<ThemeDef> availableThemes() const;

    // Index bazlı tema uygula (SettingsDialog vb için)
    void applyTheme(int index);

    // Dinamik tema uygula
    void applyTheme(ThemeMode mode, const QColor& primaryColor);

    // SVG İkonlarını temaya uygun şekilde (Açık/Koyu) renklendirir
    static QIcon icon(const QString& path);

    ThemeMode currentMode() const { return m_currentMode; }

    QColor successColor() const { return m_successColor; }
    QColor errorColor() const { return m_errorColor; }
    QColor warningColor() const { return m_warningColor; }
    QColor onlineBg() const { return m_onlineBg; }
    QColor offlineBg() const { return m_offlineBg; }

signals:
    void themeChanged();

private:
    explicit ThemeManager(QObject* parent = nullptr);
    ~ThemeManager() override = default;

    QColor calculateContrastText(const QColor& background) const;


    QApplication* m_app = nullptr;
    QList<ThemeDef> m_themes;

    QColor m_successColor;
    QColor m_errorColor;
    QColor m_warningColor;
    QColor m_onlineBg;
    QColor m_offlineBg;

    ThemeMode m_currentMode = ThemeMode::Dark;
};

#endif // THEMEMANAGER_H
