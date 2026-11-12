#pragma once

#include <QDialog>

class QLabel;
class QTextBrowser;
class QPushButton;

/// Shows a passage and its Tamil translation; stays open (non-modal) for the next one.
class TranslationDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TranslationDialog(QWidget *parent = nullptr);
    void translate(const QString &text);
    QString translation() const { return m_result; }

signals:
    void finished(bool ok);

private:
    QTextBrowser *m_source;
    QTextBrowser *m_target;
    QLabel *m_status;
    QPushButton *m_copy;
    QString m_result;
    quint64 m_request = 0;
};
