#include "ui/AccountDialog.h"

#include "auth/PasswordHash.h"
#include "auth/UserStore.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

AccountDialog::AccountDialog(auth::UserStore* users, bool firstAdmin, QWidget* parent)
    : QDialog(parent)
    , m_users(users)
    , m_firstAdmin(firstAdmin)
{
    setWindowTitle(firstAdmin ? tr("Create the administrator account")
                              : tr("Add an account"));
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    auto* intro = new QLabel(
        firstAdmin
            ? tr("NestDrive has no accounts yet. This one will be the administrator.")
            : tr("Accounts can sign in from any device on your network."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout();

    m_username = new QLineEdit(this);
    m_username->setPlaceholderText(tr("letters and numbers"));
    form->addRow(tr("Username"), m_username);

    m_password = new QLineEdit(this);
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(
        tr("at least %1 characters").arg(auth::MinPasswordLength));
    form->addRow(tr("Password"), m_password);

    m_confirm = new QLineEdit(this);
    m_confirm->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Confirm"), m_confirm);

    m_role = new QComboBox(this);
    // The stored data is the role string, so reordering this list cannot silently
    // change which role an index means.
    m_role->addItem(tr("Administrator — full access"), QStringLiteral("admin"));
    m_role->addItem(tr("Family — read and upload, delete own files"),
                    QStringLiteral("family"));
    m_role->addItem(tr("Guest — only folders you grant"), QStringLiteral("guest"));

    if (firstAdmin) {
        m_role->setCurrentIndex(0);
        m_role->setEnabled(false);
    }
    form->addRow(tr("Role"), m_role);

    layout->addLayout(form);

    m_message = new QLabel(this);
    m_message->setWordWrap(true);
    m_message->setVisible(false);
    layout->addWidget(m_message);

    auto* buttons = new QDialogButtonBox(this);
    m_create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);

    // No cancel button on first run, so creating the account is the obvious action.
    // Leaving is still possible via Escape or the window's close button, which
    // reject() turns into an explicit question rather than a silent exit.
    if (!firstAdmin)
        m_cancel = buttons->addButton(QDialogButtonBox::Cancel);

    layout->addWidget(buttons);

    connect(m_create, &QPushButton::clicked, this, &AccountDialog::onCreateClicked);
    if (m_cancel)
        connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);

    // Enter in any field creates the account rather than doing nothing.
    for (QLineEdit* field : { m_username, m_password, m_confirm })
        connect(field, &QLineEdit::returnPressed, this, &AccountDialog::onCreateClicked);

    resize(420, sizeHint().height());
}

AccountDialog::~AccountDialog()
{
    // The worker captures `this`, so it must finish before the dialog dies.
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
    }
}

void AccountDialog::reject()
{
    // A hashing worker holds `this`, and accepting a dismissal now would start
    // tearing the dialog down while it runs. Waiting is wrong too — it would freeze
    // the window for seconds — so the dismissal is simply ignored until it finishes,
    // which is at most a few seconds away and is already explained on screen.
    if (m_worker && m_worker->isRunning())
        return;

    if (m_firstAdmin) {
        const auto answer = QMessageBox::question(
            this, tr("Continue without an account?"),
            tr("Without an account nobody can sign in, and no file will be reachable "
               "from another device — every request is sent to the login page.\n\n"
               "You can create one later with the Accounts button."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return;
    }

    QDialog::reject();
}

QString AccountDialog::validationError() const
{
    const QString username = m_username->text().trimmed();

    if (username.isEmpty())
        return tr("Enter a username.");

    // Rejected here rather than at the database: a name with a space or a slash in
    // it is confusing to type on a phone and buys nothing.
    for (const QChar character : username) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('-')
            && character != QLatin1Char('_') && character != QLatin1Char('.')) {
            return tr("Usernames may contain letters, numbers, dot, dash and "
                      "underscore only.");
        }
    }

    if (m_password->text().length() < auth::MinPasswordLength) {
        return tr("The password must be at least %1 characters.")
            .arg(auth::MinPasswordLength);
    }

    if (m_password->text() != m_confirm->text())
        return tr("The two passwords do not match.");

    return QString();
}

void AccountDialog::showMessage(const QString& text, bool isError)
{
    m_message->setText(text);
    m_message->setStyleSheet(isError ? QStringLiteral("color: #c0392b;")
                                     : QStringLiteral("color: #888;"));
    m_message->setVisible(!text.isEmpty());
}

void AccountDialog::setBusy(bool busy)
{
    m_create->setEnabled(!busy);
    if (m_cancel)
        m_cancel->setEnabled(!busy);

    m_username->setEnabled(!busy);
    m_password->setEnabled(!busy);
    m_confirm->setEnabled(!busy);
    m_role->setEnabled(!busy && !m_firstAdmin);
}

void AccountDialog::onCreateClicked()
{
    if (m_worker && m_worker->isRunning())
        return;                            // Already working; ignore a second click.

    if (!m_users) {
        showMessage(tr("Accounts are unavailable because the database did not open."),
                    true);
        return;
    }

    const QString problem = validationError();
    if (!problem.isEmpty()) {
        showMessage(problem, true);
        return;
    }

    const QString username = m_username->text().trimmed();
    const QString password = m_password->text();

    // In first-run mode the role is not read from the widget at all. The combo is
    // disabled and set to admin, so reading it would normally agree — but the
    // fallback below is Guest, and a first "administrator" created as a guest would
    // be an installation nobody can administer, with nothing on screen to say so.
    const auto role = m_firstAdmin
                          ? auth::Role::Admin
                          : auth::roleFromString(m_role->currentData().toString())
                                .value_or(auth::Role::Guest);

    setBusy(true);
    showMessage(tr("Creating the account. This takes a few seconds — the password "
                   "is deliberately slow to hash."), false);

    // Joined and replaced rather than leaked: a dialog reopened after a failed
    // attempt would otherwise accumulate threads.
    if (m_worker) {
        m_worker->wait();
        delete m_worker;
        m_worker = nullptr;
    }

    // Owned by the lambdas below. The id is carried out alongside the error because
    // the audit entry needs it, and looking the account up again afterwards would be
    // a second query for something this call already returned.
    struct Outcome { QString error; int id = 0; };
    auto* result = new Outcome;

    m_worker = QThread::create([this, username, password, role, result]() {
        QString error;
        const int id = m_users->create(username, password, role, &error);
        result->id    = id;
        result->error = (id > 0) ? QString() : error;
    });

    connect(m_worker, &QThread::finished, this, [this, username, result]() {
        const QString error = result->error;
        const int     id    = result->id;
        delete result;

        setBusy(false);

        if (!error.isEmpty()) {
            showMessage(error, true);
            m_password->clear();
            m_confirm->clear();
            m_password->setFocus();
            return;
        }

        m_createdUsername = auth::UserStore::normaliseUsername(username);
        m_createdUserId   = id;
        accept();
    });

    m_worker->start();
}
