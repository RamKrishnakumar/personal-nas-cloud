#include <QTest>

#include <QSqlQuery>
#include <QTemporaryDir>

#include <algorithm>

#include "auth/PermissionStore.h"
#include "auth/Permissions.h"
#include "auth/UserStore.h"
#include "db/Database.h"

using namespace auth;

namespace {

UserRecord makeUser(int id, Role role, bool enabled = true)
{
    UserRecord user;
    user.id       = id;
    user.username = QStringLiteral("u%1").arg(id);
    user.role     = role;
    user.enabled  = enabled;
    return user;
}

FolderPerm grantOf(const QString& prefix, PermLevel level)
{
    return FolderPerm{ prefix, level };
}

} // namespace

/**
 * Tests for auth::resolve and auth::PermissionStore.
 *
 * The plan calls the permission suite one of the two that matter most, alongside
 * path traversal, because a bug in either is a disclosure bug and neither is
 * visible in manual testing: a guest who can see one extra folder looks exactly
 * like a guest who cannot.
 *
 * The three properties worth stating outright:
 *   - a guest with no grants gets nothing, rather than everything
 *   - an explicit `none` is not overridable by a permitting ancestor
 *   - a prefix match stops at a separator, so `/Photo` does not cover `/Photos`
 */
class TestPermissions : public QObject
{
    Q_OBJECT

private slots:
    // ── Path normalisation ───────────────────────────────────────────────
    void normalisesPaths_data();
    void normalisesPaths();

    // ── Default, with no grants at all ───────────────────────────────────
    void adminWithNoGrantsHasFullAccess();
    void familyWithNoGrantsCanReadAndWrite();
    void guestWithNoGrantsHasNothing();
    void disabledAccountHasNothingWhateverTheRole_data();
    void disabledAccountHasNothingWhateverTheRole();
    void invalidUserIdHasNothing();

    // ── Prefix matching ──────────────────────────────────────────────────
    void exactMatchApplies();
    void ancestorGrantCoversDescendants();
    void prefixDoesNotLeakAcrossASiblingName();
    void longerPrefixWinsOverShorter();
    void orderOfGrantsDoesNotMatter();
    void rootGrantCoversEverything();
    void trailingSlashInAGrantIsIgnored();

    // ── Explicit denial ──────────────────────────────────────────────────
    void explicitNoneDeniesBeneathAPermittingAncestor();
    void explicitNoneDeniesTheWholeSubtree();
    void explicitNoneDeniesAnAdminToo();
    void aLongerPermitBeatsAShorterDeny();

    // ── Removal ──────────────────────────────────────────────────────────
    void adminMayRemove();
    void familyMayRemoveOnlyTheirOwnUploads();
    void familyMayNotRemoveAFileWithNoRecordedOwner();
    void guestMayNeverRemove();
    void removalRequiresWrite();

    // ── Store ────────────────────────────────────────────────────────────
    void storeRoundTripsGrants();
    void storeNormalisesThePrefix();
    void storeReplacesAnExistingGrant();
    void storeRevokes();
    void storeIsolatesUsers();
    void storeDropsGrantsWithAUser();
};

// ─────────────────────────────────────────────────────────────────────────────
//  Normalisation
// ─────────────────────────────────────────────────────────────────────────────

void TestPermissions::normalisesPaths_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("empty")            << QString()                  << "/";
    QTest::newRow("root")             << "/"                        << "/";
    QTest::newRow("double root")      << "//"                       << "/";
    QTest::newRow("no leading slash") << "Photos"                   << "/Photos";
    QTest::newRow("trailing slash")   << "/Photos/"                 << "/Photos";
    QTest::newRow("many trailing")    << "/Photos///"               << "/Photos";
    QTest::newRow("doubled inner")    << "/Photos//2024"            << "/Photos/2024";
    QTest::newRow("dot segment")      << "/Photos/./2024"           << "/Photos/2024";
    QTest::newRow("backslashes")      << "\\Photos\\2024"           << "/Photos/2024";
    QTest::newRow("climbs out")       << "/.."                      << "/";
    QTest::newRow("climbs further")   << "/../../etc"               << "/";
    QTest::newRow("inner climb")      << "/Photos/../Private"       << "/Private";
}

void TestPermissions::normalisesPaths()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    QCOMPARE(normalisePermissionPath(input), expected);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Defaults
// ─────────────────────────────────────────────────────────────────────────────

void TestPermissions::adminWithNoGrantsHasFullAccess()
{
    const Perm perm = resolve(makeUser(1, Role::Admin), {}, QStringLiteral("/Photos"));

    QVERIFY(perm.read);
    QVERIFY(perm.write);
    QVERIFY(perm.remove);
}

void TestPermissions::familyWithNoGrantsCanReadAndWrite()
{
    const Perm perm = resolve(makeUser(2, Role::Family), {}, QStringLiteral("/Photos"));

    QVERIFY(perm.read);
    QVERIFY(perm.write);

    // Removal needs an owner match, and no file was named.
    QVERIFY(!perm.remove);
}

void TestPermissions::guestWithNoGrantsHasNothing()
{
    // The property that makes the guest role meaningful. If this ever inverts, every
    // guest silently gains the whole library.
    for (const QString& path : { QStringLiteral("/"),
                                 QStringLiteral("/Photos"),
                                 QStringLiteral("/Photos/2024/holiday.jpg") }) {
        const Perm perm = resolve(makeUser(3, Role::Guest), {}, path);

        QVERIFY2(!perm.read, qPrintable(path));
        QVERIFY2(!perm.write, qPrintable(path));
        QVERIFY2(!perm.remove, qPrintable(path));
    }
}

void TestPermissions::disabledAccountHasNothingWhateverTheRole_data()
{
    QTest::addColumn<int>("role");

    QTest::newRow("admin")  << int(Role::Admin);
    QTest::newRow("family") << int(Role::Family);
    QTest::newRow("guest")  << int(Role::Guest);
}

void TestPermissions::disabledAccountHasNothingWhateverTheRole()
{
    QFETCH(int, role);

    // Disabling must outrank every grant, including an explicit write on the root.
    // Anything else means turning an account off does not turn it off.
    const QList<FolderPerm> generous = { grantOf(QStringLiteral("/"), PermLevel::Write) };

    const Perm perm = resolve(makeUser(4, Role(role), /*enabled=*/false),
                              generous, QStringLiteral("/Photos"));

    QVERIFY(!perm.read);
    QVERIFY(!perm.write);
    QVERIFY(!perm.remove);
}

void TestPermissions::invalidUserIdHasNothing()
{
    // A default-constructed UserRecord must not resolve to anything, because that is
    // what an un-gated handler would be holding.
    UserRecord nobody;
    const Perm perm = resolve(nobody, {}, QStringLiteral("/Photos"));

    QVERIFY(!perm.read);
    QVERIFY(!perm.write);
    QVERIFY(!perm.remove);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Prefix matching
// ─────────────────────────────────────────────────────────────────────────────

void TestPermissions::exactMatchApplies()
{
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Shared"), PermLevel::Read)
    };

    const Perm perm = resolve(makeUser(3, Role::Guest), grants,
                              QStringLiteral("/Shared"));
    QVERIFY(perm.read);
    QVERIFY(!perm.write);
}

void TestPermissions::ancestorGrantCoversDescendants()
{
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Shared"), PermLevel::Read)
    };

    QVERIFY(resolve(makeUser(3, Role::Guest), grants,
                    QStringLiteral("/Shared/2024")).read);
    QVERIFY(resolve(makeUser(3, Role::Guest), grants,
                    QStringLiteral("/Shared/2024/deep/photo.jpg")).read);
}

void TestPermissions::prefixDoesNotLeakAcrossASiblingName()
{
    // A grant on /Photo must not cover /Photos. This is the bug that a naive
    // startsWith() produces, and it hands over a whole unrelated folder.
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Photo"), PermLevel::Write)
    };

    QVERIFY(resolve(makeUser(3, Role::Guest), grants, QStringLiteral("/Photo")).read);
    QVERIFY2(!resolve(makeUser(3, Role::Guest), grants, QStringLiteral("/Photos")).read,
             "A grant on /Photo leaked into the sibling folder /Photos");
    QVERIFY(!resolve(makeUser(3, Role::Guest), grants,
                     QStringLiteral("/Photos/2024")).read);
}

void TestPermissions::longerPrefixWinsOverShorter()
{
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/"), PermLevel::Read),
        grantOf(QStringLiteral("/Shared"), PermLevel::Write),
    };

    const Perm root = resolve(makeUser(3, Role::Guest), grants, QStringLiteral("/"));
    QVERIFY(root.read);
    QVERIFY(!root.write);

    const Perm shared = resolve(makeUser(3, Role::Guest), grants,
                                QStringLiteral("/Shared/x"));
    QVERIFY(shared.read);
    QVERIFY(shared.write);
}

void TestPermissions::orderOfGrantsDoesNotMatter()
{
    // Precedence must come from the prefixes, not from the order rows happened to
    // come back from the database.
    const QList<FolderPerm> ascending = {
        grantOf(QStringLiteral("/"), PermLevel::Read),
        grantOf(QStringLiteral("/A/B"), PermLevel::Write),
        grantOf(QStringLiteral("/A"), PermLevel::None),
    };
    QList<FolderPerm> descending = ascending;
    std::reverse(descending.begin(), descending.end());

    const UserRecord guest = makeUser(3, Role::Guest);

    for (const QString& path : { QStringLiteral("/"),
                                 QStringLiteral("/A"),
                                 QStringLiteral("/A/B"),
                                 QStringLiteral("/A/C") }) {
        const Perm first  = resolve(guest, ascending, path);
        const Perm second = resolve(guest, descending, path);

        QCOMPARE(first.read, second.read);
        QCOMPARE(first.write, second.write);
    }
}

void TestPermissions::rootGrantCoversEverything()
{
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/"), PermLevel::Read)
    };

    QVERIFY(resolve(makeUser(3, Role::Guest), grants, QStringLiteral("/")).read);
    QVERIFY(resolve(makeUser(3, Role::Guest), grants,
                    QStringLiteral("/anything/at/all.jpg")).read);
}

void TestPermissions::trailingSlashInAGrantIsIgnored()
{
    // Two spellings of one folder must not produce different precedence, or a grant
    // typed with a slash would silently outrank a more specific one.
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Shared/"), PermLevel::Read),
        grantOf(QStringLiteral("/Shared/Private"), PermLevel::None),
    };

    QVERIFY(resolve(makeUser(3, Role::Guest), grants, QStringLiteral("/Shared")).read);
    QVERIFY(!resolve(makeUser(3, Role::Guest), grants,
                     QStringLiteral("/Shared/Private")).read);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Explicit denial
// ─────────────────────────────────────────────────────────────────────────────

void TestPermissions::explicitNoneDeniesBeneathAPermittingAncestor()
{
    // The case the plan singles out. Deny has to be un-overridable, or "share this
    // folder except that subfolder" cannot be expressed at all.
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Shared"), PermLevel::Write),
        grantOf(QStringLiteral("/Shared/Private"), PermLevel::None),
    };

    const UserRecord guest = makeUser(3, Role::Guest);

    QVERIFY(resolve(guest, grants, QStringLiteral("/Shared")).read);
    QVERIFY(resolve(guest, grants, QStringLiteral("/Shared/Public")).read);

    const Perm denied = resolve(guest, grants, QStringLiteral("/Shared/Private"));
    QVERIFY2(!denied.read, "An explicit none was overridden by a permitting ancestor");
    QVERIFY(!denied.write);
    QVERIFY(!denied.remove);
}

void TestPermissions::explicitNoneDeniesTheWholeSubtree()
{
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Shared"), PermLevel::Write),
        grantOf(QStringLiteral("/Shared/Private"), PermLevel::None),
    };

    // Denying a folder but not its contents would be worthless.
    QVERIFY(!resolve(makeUser(3, Role::Guest), grants,
                     QStringLiteral("/Shared/Private/deep/secret.jpg")).read);
}

void TestPermissions::explicitNoneDeniesAnAdminToo()
{
    // A deliberate deviation from the plan's literal "admin always" for removal: an
    // explicit denial that silently did not apply to an administrator would be a
    // trap for whoever set it.
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Vault"), PermLevel::None)
    };

    const Perm perm = resolve(makeUser(1, Role::Admin), grants,
                              QStringLiteral("/Vault/x.jpg"));

    QVERIFY(!perm.read);
    QVERIFY(!perm.write);
    QVERIFY2(!perm.remove, "An explicit deny did not stop an administrator deleting");
}

void TestPermissions::aLongerPermitBeatsAShorterDeny()
{
    // Denial wins at equal specificity, but not against a more specific permit —
    // otherwise "deny everything except this one folder" could not be written.
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/"), PermLevel::None),
        grantOf(QStringLiteral("/Shared/Album"), PermLevel::Read),
    };

    const UserRecord guest = makeUser(3, Role::Guest);

    QVERIFY(!resolve(guest, grants, QStringLiteral("/")).read);
    QVERIFY(!resolve(guest, grants, QStringLiteral("/Shared")).read);
    QVERIFY(resolve(guest, grants, QStringLiteral("/Shared/Album")).read);
    QVERIFY(resolve(guest, grants, QStringLiteral("/Shared/Album/x.jpg")).read);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Removal
// ─────────────────────────────────────────────────────────────────────────────

void TestPermissions::adminMayRemove()
{
    const UserRecord admin = makeUser(1, Role::Admin);

    // Regardless of who uploaded it, including nobody.
    QVERIFY(resolve(admin, {}, QStringLiteral("/x.jpg"), 99).remove);
    QVERIFY(resolve(admin, {}, QStringLiteral("/x.jpg"), 1).remove);
    QVERIFY(resolve(admin, {}, QStringLiteral("/x.jpg"), std::nullopt).remove);
}

void TestPermissions::familyMayRemoveOnlyTheirOwnUploads()
{
    const UserRecord family = makeUser(7, Role::Family);

    QVERIFY2(resolve(family, {}, QStringLiteral("/x.jpg"), 7).remove,
             "A family member cannot delete their own upload");
    QVERIFY2(!resolve(family, {}, QStringLiteral("/x.jpg"), 8).remove,
             "A family member can delete someone else's upload");
}

void TestPermissions::familyMayNotRemoveAFileWithNoRecordedOwner()
{
    // Files that predate the index have no uploader. Guessing permissively would
    // make every pre-existing file deletable by anyone in the family.
    const UserRecord family = makeUser(7, Role::Family);

    QVERIFY(!resolve(family, {}, QStringLiteral("/old.jpg"), std::nullopt).remove);
}

void TestPermissions::guestMayNeverRemove()
{
    // Even with write granted, and even for a file they uploaded themselves.
    const QList<FolderPerm> grants = {
        grantOf(QStringLiteral("/Drop"), PermLevel::Write)
    };
    const UserRecord guest = makeUser(3, Role::Guest);

    const Perm perm = resolve(guest, grants, QStringLiteral("/Drop/x.jpg"), 3);
    QVERIFY(perm.read);
    QVERIFY(perm.write);
    QVERIFY2(!perm.remove, "A guest was allowed to delete");
}

void TestPermissions::removalRequiresWrite()
{
    // Read-only means read-only, for every role.
    const QList<FolderPerm> readOnly = {
        grantOf(QStringLiteral("/"), PermLevel::Read)
    };

    for (Role role : { Role::Admin, Role::Family, Role::Guest }) {
        const Perm perm = resolve(makeUser(5, role), readOnly,
                                  QStringLiteral("/x.jpg"), 5);
        QVERIFY(perm.read);
        QVERIFY(!perm.write);
        QVERIFY(!perm.remove);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Store
// ─────────────────────────────────────────────────────────────────────────────

void TestPermissions::storeRoundTripsGrants()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    db::Database database;
    QVERIFY2(database.open(tmp.path() + QStringLiteral("/db.sqlite")),
             qPrintable(database.lastError()));

    UserStore users(&database);
    users.setIterationsForTesting(1000);

    QString error;
    const int id = users.create(QStringLiteral("guest1"),
                                QStringLiteral("a-good-password"),
                                Role::Guest, &error);
    QVERIFY2(id > 0, qPrintable(error));

    PermissionStore perms(&database);
    QVERIFY(perms.grant(id, QStringLiteral("/Shared"), PermLevel::Read));
    QVERIFY(perms.grant(id, QStringLiteral("/Shared/Private"), PermLevel::None));

    const QList<FolderPerm> loaded = perms.forUser(id);
    QCOMPARE(loaded.size(), 2);

    // Loaded grants must resolve exactly as hand-built ones do.
    const auto user = users.findById(id);
    QVERIFY(user.has_value());
    QVERIFY(resolve(*user, loaded, QStringLiteral("/Shared")).read);
    QVERIFY(!resolve(*user, loaded, QStringLiteral("/Shared/Private")).read);
    QVERIFY(!resolve(*user, loaded, QStringLiteral("/Elsewhere")).read);
}

void TestPermissions::storeNormalisesThePrefix()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    db::Database database;
    QVERIFY(database.open(tmp.path() + QStringLiteral("/db.sqlite")));

    UserStore users(&database);
    users.setIterationsForTesting(1000);
    QString error;
    const int id = users.create(QStringLiteral("g"), QStringLiteral("a-good-password"),
                                Role::Guest, &error);
    QVERIFY(id > 0);

    PermissionStore perms(&database);
    QVERIFY(perms.grant(id, QStringLiteral("Shared//Album/"), PermLevel::Read));

    const QList<FolderPerm> loaded = perms.forUser(id);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().pathPrefix, QStringLiteral("/Shared/Album"));
}

void TestPermissions::storeReplacesAnExistingGrant()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    db::Database database;
    QVERIFY(database.open(tmp.path() + QStringLiteral("/db.sqlite")));

    UserStore users(&database);
    users.setIterationsForTesting(1000);
    QString error;
    const int id = users.create(QStringLiteral("g"), QStringLiteral("a-good-password"),
                                Role::Guest, &error);
    QVERIFY(id > 0);

    PermissionStore perms(&database);
    QVERIFY(perms.grant(id, QStringLiteral("/Shared"), PermLevel::Read));

    // Two spellings of the same folder must update one row, not create a second
    // that silently contradicts the first.
    QVERIFY(perms.grant(id, QStringLiteral("/Shared/"), PermLevel::None));

    const QList<FolderPerm> loaded = perms.forUser(id);
    QCOMPARE(loaded.size(), 1);
    QCOMPARE(loaded.first().level, PermLevel::None);
}

void TestPermissions::storeRevokes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    db::Database database;
    QVERIFY(database.open(tmp.path() + QStringLiteral("/db.sqlite")));

    UserStore users(&database);
    users.setIterationsForTesting(1000);
    QString error;
    const int id = users.create(QStringLiteral("g"), QStringLiteral("a-good-password"),
                                Role::Guest, &error);
    QVERIFY(id > 0);

    PermissionStore perms(&database);
    QVERIFY(perms.grant(id, QStringLiteral("/Shared"), PermLevel::Read));
    QVERIFY(perms.revoke(id, QStringLiteral("/Shared")));
    QVERIFY(perms.forUser(id).isEmpty());

    QVERIFY(!perms.revoke(id, QStringLiteral("/Shared")));   // Nothing left.
}

void TestPermissions::storeIsolatesUsers()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    db::Database database;
    QVERIFY(database.open(tmp.path() + QStringLiteral("/db.sqlite")));

    UserStore users(&database);
    users.setIterationsForTesting(1000);
    QString error;
    const int first  = users.create(QStringLiteral("a"),
                                    QStringLiteral("a-good-password"),
                                    Role::Guest, &error);
    const int second = users.create(QStringLiteral("b"),
                                    QStringLiteral("a-good-password"),
                                    Role::Guest, &error);
    QVERIFY(first > 0 && second > 0);

    PermissionStore perms(&database);
    QVERIFY(perms.grant(first, QStringLiteral("/OnlyMine"), PermLevel::Write));

    // One guest's grant must not reach another's. This is the store half of guest
    // isolation; resolve() is the other half.
    QCOMPARE(perms.forUser(first).size(), 1);
    QVERIFY2(perms.forUser(second).isEmpty(),
             "A grant leaked from one user to another");
}

void TestPermissions::storeDropsGrantsWithAUser()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    db::Database database;
    QVERIFY(database.open(tmp.path() + QStringLiteral("/db.sqlite")));

    UserStore users(&database);
    users.setIterationsForTesting(1000);
    QString error;
    const int id = users.create(QStringLiteral("g"), QStringLiteral("a-good-password"),
                                Role::Guest, &error);
    QVERIFY(id > 0);

    PermissionStore perms(&database);
    QVERIFY(perms.grant(id, QStringLiteral("/Shared"), PermLevel::Read));

    // ON DELETE CASCADE, so a recreated user cannot inherit a deleted one's rights
    // by landing on the same row id.
    {
        auto handle = database.lock();
        QSqlQuery query(handle.connection());
        query.prepare(QStringLiteral("DELETE FROM users WHERE id = ?"));
        query.addBindValue(id);
        QVERIFY(query.exec());
    }

    QVERIFY2(perms.forUser(id).isEmpty(),
             "Grants outlived the account they belonged to");
}

QTEST_MAIN(TestPermissions)
#include "tst_permissions.moc"
