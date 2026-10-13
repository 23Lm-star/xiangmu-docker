// captcha_simple.h
#ifndef CAPTCHA_SIMPLE_H
#define CAPTCHA_SIMPLE_H

#include <QWidget>
#include <QLabel>
#include <QString>

class SimpleCaptcha : public QWidget
{
    Q_OBJECT
public:
    explicit SimpleCaptcha(QWidget *parent = nullptr);

    QString getCaptchaCode() const { return m_code; }
    void setCodeLength(int length);
    bool verify(const QString &input) const;
    void refreshCaptcha();   // 刷新验证码

signals:
    void captchaRefreshed();

private:
    QLabel *m_label;         // 用于显示验证码文本
    QString m_code;          // 当前验证码
    int m_codeLength;        // 验证码长度

    QString generateRandomCode(); // 生成随机验证码
};

#endif // CAPTCHA_SIMPLE_H
