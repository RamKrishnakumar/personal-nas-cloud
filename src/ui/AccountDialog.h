#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QThread;

namespace auth { class UserStore; }

/**
 * @class AccountDialog
 * @brief Creates one account, hashing its password off the GUI thread.
 *
 * ### Why this is threaded for a single dialog
 *
 * Hashing a password costs 1.4 s on a modern laptop and an estimated 3–4 s on the
 * reference i5 6th gen. A window frozen that long reads as a crash, and the first
 * thing a person does with an unresponsive window is click again. The work
 * therefore runs on a short-lived thread while the dialog stays painted and says
 * what it is doing.
 *
 * ### First-run mode
 *
 * When no account exists the role is fixed to admin and the dialog cannot be
 * dismissed into a half-configured state — an installation with accounts but no
 * administrator has nobody who can create one.
 */
class AccountDialog : public QDialog
{
    Q_OBJECT

public:
    /**
     * @param users          Account store. Must outlive the dialog.
     * @param firstAdmin     Fixes the role to admin and adapts the wording.
     */
    AccountDialog(auth::UserStore* users, bool firstAdmin, QWidget* parent = nullptr);
    ~AccountDialog() override;

    /** Username that was created, once the dialog was accepted. */
    QString createdUsername() const { return m_createdUsername; }

private slots:
    void onCreateClicked();

private:
    /** Validates the fields and returns a reason when they are not usable. */
    QString validationError() const;

    void setBusy(bool busy);
    void showMessage(const QString& text, bool isError);

    auth::UserStore* m_users = nullptr;
    bool             m_firstAdmin = false;

    QLineEdit*   m_username  = nullptr;
    QLineEdit*   m_password  = nullptr;
    QLineEdit*   m_confirm   = nullptr;
    QComboBox*   m_role      = nullptr;
    QPushButton* m_create    = nullptr;
    QPushButton* m_cancel    = nullptr;
    QLabel*      m_message   = nullptr;

    QThread* m_worker = nullptr;
    QString  m_createdUsername;
};
