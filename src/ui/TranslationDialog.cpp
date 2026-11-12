#include "ui/TranslationDialog.h"

#include "net/Translator.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

TranslationDialog::TranslationDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Translate to தமிழ்");
    setModal(false);
    resize(560, 360);
    auto *layout = new QVBoxLayout(this);

    auto *srcLabel = new QLabel("<b>English</b>");
    m_source = new QTextBrowser;
    m_source->setMaximumHeight(110);
    auto *tgtLabel = new QLabel("<b>தமிழ்</b>");
    m_target = new QTextBrowser;
    QFont f = m_target->font();
    f.setPointSizeF(f.pointSizeF() * 1.3);
    f.setFamilies({"Noto Sans Tamil", "Noto Serif Tamil", "Latha", "Nirmala UI", f.family()});
    m_target->setFont(f);
    m_status = new QLabel;
    m_status->setStyleSheet("color: palette(placeholder-text);");

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    m_copy = buttons->addButton("Copy Tamil", QDialogButtonBox::ActionRole);
    auto *copyBoth = buttons->addButton("Copy both", QDialogButtonBox::ActionRole);
    connect(m_copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_result); });
    connect(copyBoth, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(m_source->toPlainText() + "\n\n" + m_result);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);

    layout->addWidget(srcLabel);
    layout->addWidget(m_source);
    layout->addWidget(tgtLabel);
    layout->addWidget(m_target, 1);
    layout->addWidget(m_status);
    layout->addWidget(buttons);
}

void TranslationDialog::translate(const QString &text)
{
    const QString clean = text.simplified().left(4500);
    m_source->setPlainText(clean);
    m_target->clear();
    m_result.clear();
    m_copy->setEnabled(false);
    m_status->setText("Translating…");
    show();
    raise();
    activateWindow();

    const quint64 id = ++m_request;
    auto *watcher = new QFutureWatcher<Translator::Result>(this);
    connect(watcher, &QFutureWatcher<Translator::Result>::finished, this, [this, watcher, id] {
        watcher->deleteLater();
        if (id != m_request)
            return; // a newer passage was asked for meanwhile
        const Translator::Result r = watcher->result();
        if (r.text.isEmpty()) {
            m_status->setText("Couldn't translate: " + r.error);
            emit finished(false);
            return;
        }
        m_result = r.text;
        m_target->setPlainText(r.text);
        m_copy->setEnabled(true);
        m_status->setText(r.cached ? "Saved translation (works offline) · Google Translate"
                                   : "Machine translation by Google Translate — may not be perfect");
        emit finished(true);
    });
    watcher->setFuture(QtConcurrent::run([clean] { return Translator::translate(clean, "ta"); }));
}
