/* === This file is part of Calamares - <https://calamares.io> ===
 *
 *   SPDX-FileCopyrightText: 2020 Adriaan de Groot <groot@kde.org>
 *   SPDX-License-Identifier: GPL-3.0-or-later
 *
 *   Calamares is Free Software: see the License-Identifier above.
 *
 */

#include "Config.h"

#include "ActiveDirectoryJob.h"
#include "CreateUserJob.h"
#include "MiscJobs.h"
#include "SetHostNameJob.h"
#include "SetPasswordJob.h"

#include "GlobalStorage.h"
#include "JobQueue.h"
#include "compat/Variant.h"
#include "utils/Logger.h"
#include "utils/Permissions.h"
#include "utils/String.h"
#include "utils/StringExpander.h"
#include "utils/Variant.h"

#include <QCoreApplication>
#include <QFile>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QTimer>

#ifdef HAVE_ICU
#include <unicode/translit.h>
#include <unicode/unistr.h>

//Needed for ICU to apply some transliteration ruleset.
//Still needs to be adjusted to fit the needs of the most of users
static const char TRANSLITERATOR_ID[] = "Russian-Latin/BGN;"
                                        "Greek-Latin/UNGEGN;"
                                        "Any-Latin;"
                                        "Latin-ASCII";
#endif

#include <memory>

static const QRegularExpression USERNAME_RX( "^[a-z_][a-z0-9_-]*[$]?$" );  // Note anchors begin and end
static constexpr const int USERNAME_MAX_LENGTH = 31;

static const QRegularExpression HOSTNAME_RX( "^[a-zA-Z0-9][-a-zA-Z0-9_]*$" );  // Note anchors begin and end
static constexpr const int HOSTNAME_MIN_LENGTH = 2;
static constexpr const int HOSTNAME_MAX_LENGTH = 63;

static void
updateGSAutoLogin( bool doAutoLogin, const QString& login )
{
    Calamares::GlobalStorage* gs = Calamares::JobQueue::instance()->globalStorage();
    if ( !gs )
    {
        cWarning() << "No Global Storage available";
        return;
    }

    if ( doAutoLogin && !login.isEmpty() )
    {
        gs->insert( "autoLoginUser", login );
    }
    else
    {
        gs->remove( "autoLoginUser" );
    }

    if ( login.isEmpty() )
    {
        gs->remove( "username" );
    }
    else
    {
        gs->insert( "username", login );
    }
}

static const QStringList&
alwaysForbiddenLoginNames()
{
    static QStringList s { QStringLiteral( "root" ), QStringLiteral( "nobody" ) };
    return s;
}

static const QStringList&
alwaysForbiddenHostNames()
{
    static QStringList s { QStringLiteral( "localhost" ) };
    return s;
}

const NamedEnumTable< HostNameAction >&
hostnameActionNames()
{
    // *INDENT-OFF*
    // clang-format off
    static const NamedEnumTable< HostNameAction > names {
        { QStringLiteral( "none" ), HostNameAction::None },
        { QStringLiteral( "etcfile" ), HostNameAction::EtcHostname },
        { QStringLiteral( "etc" ), HostNameAction::EtcHostname },
        { QStringLiteral( "hostnamed" ), HostNameAction::SystemdHostname },
        { QStringLiteral( "transient" ), HostNameAction::Transient },
    };
    // clang-format on
    // *INDENT-ON*

    return names;
}

Config::Config( QObject* parent )
    : Calamares::ModuleSystem::Config( parent )
    , m_forbiddenHostNames( alwaysForbiddenHostNames() )
    , m_forbiddenLoginNames( alwaysForbiddenLoginNames() )
{
    emit readyChanged( m_isReady );  // false

    // Gang together all the changes of status to one readyChanged() signal
    connect( this, &Config::hostnameStatusChanged, this, &Config::checkReady );
    connect( this, &Config::loginNameStatusChanged, this, &Config::checkReady );
    connect( this, &Config::fullNameChanged, this, &Config::checkReady );
    connect( this, &Config::userPasswordStatusChanged, this, &Config::checkReady );
    connect( this, &Config::rootPasswordStatusChanged, this, &Config::checkReady );
    connect( this, &Config::reuseUserPasswordForRootChanged, this, &Config::checkReady );
    connect( this, &Config::requireStrongPasswordsChanged, this, &Config::checkReady );
}

Config::~Config() {}

void
Config::setUserShell( const QString& shell )
{
    if ( !shell.isEmpty() && !shell.startsWith( '/' ) )
    {
        cWarning() << "User shell" << shell << "is not an absolute path.";
        return;
    }
    if ( shell != m_userShell )
    {
        m_userShell = shell;
        emit userShellChanged( shell );
        // The shell is put into GS as well.
        auto* gs = Calamares::JobQueue::instance()->globalStorage();
        if ( gs )
        {
            gs->insert( "userShell", shell );
        }
    }
}

static inline void
insertInGlobalStorage( const QString& key, const QString& group )
{
    auto* gs = Calamares::JobQueue::instance()->globalStorage();
    if ( !gs || group.isEmpty() )
    {
        return;
    }
    gs->insert( key, group );
}

void
Config::setAutoLoginGroup( const QString& group )
{
    if ( group != m_autoLoginGroup )
    {
        m_autoLoginGroup = group;
        insertInGlobalStorage( QStringLiteral( "autoLoginGroup" ), group );
        emit autoLoginGroupChanged( group );
    }
}

QStringList
Config::groupsForThisUser() const
{
    QStringList l;
    l.reserve( defaultGroups().size() + 1 );

    for ( const auto& g : defaultGroups() )
    {
        l << g.name();
    }
    if ( doAutoLogin() && !autoLoginGroup().isEmpty() )
    {
        l << autoLoginGroup();
    }

    return l;
}

void
Config::setSudoersGroup( const QString& group )
{
    if ( group != m_sudoersGroup )
    {
        m_sudoersGroup = group;
        insertInGlobalStorage( QStringLiteral( "sudoersGroup" ), group );
        emit sudoersGroupChanged( group );
    }
}

void
Config::setLoginName( const QString& login )
{
    CONFIG_PREVENT_EDITING( QString, "loginName" );
    if ( login != m_loginName )
    {
        m_customLoginName = !login.isEmpty();
        m_loginName = login;
        updateGSAutoLogin( doAutoLogin(), login );
        emit loginNameChanged( login );
        emit loginNameStatusChanged( loginNameStatus() );
    }
}

const QStringList&
Config::forbiddenLoginNames() const
{
    return m_forbiddenLoginNames;
}

QString
Config::loginNameStatus() const
{
    // An empty login is "ok", even if it isn't really
    if ( m_loginName.isEmpty() )
    {
        return QString();
    }

    if ( m_loginName.length() > USERNAME_MAX_LENGTH )
    {
        return tr( "Your username is too long." );
    }

    QRegularExpression validateFirstLetter( "^[a-z_]" );
    if ( m_loginName.indexOf( validateFirstLetter ) != 0 )
    {
        return tr( "Your username must start with a lowercase letter or underscore." );
    }
    if ( m_loginName.indexOf( USERNAME_RX ) != 0 )
    {
        return tr( "Only lowercase letters, numbers, underscore and hyphen are allowed." );
    }

    // Although we've made the list lower-case, and the RE above forces lower-case, still pass the flag
    if ( forbiddenLoginNames().contains( m_loginName, Qt::CaseInsensitive ) )
    {
        return tr( "'%1' is not allowed as username." ).arg( m_loginName );
    }

    return QString();
}

void
Config::setHostName( const QString& host )
{
    if ( hostnameAction() != HostNameAction::EtcHostname && hostnameAction() != HostNameAction::SystemdHostname )
    {
        cDebug() << "Ignoring hostname" << host << "No hostname will be set.";
        return;
    }
    if ( host != m_hostname )
    {
        m_customHostName = !host.isEmpty();
        m_hostname = host;
        Calamares::GlobalStorage* gs = Calamares::JobQueue::instance()->globalStorage();
        if ( host.isEmpty() )
        {
            gs->remove( "hostname" );
        }
        else
        {
            gs->insert( "hostname", host );
        }
        emit hostnameChanged( host );
        emit hostnameStatusChanged( hostnameStatus() );
    }
}

const QStringList&
Config::forbiddenHostNames() const
{
    return m_forbiddenHostNames;
}

QString
Config::hostnameStatus() const
{
    // An empty hostname is "ok", even if it isn't really
    if ( m_hostname.isEmpty() )
    {
        return QString();
    }

    if ( m_hostname.length() < HOSTNAME_MIN_LENGTH )
    {
        return tr( "Your hostname is too short." );
    }
    if ( m_hostname.length() > HOSTNAME_MAX_LENGTH )
    {
        return tr( "Your hostname is too long." );
    }

    // "LocalHost" is just as forbidden as "localhost"
    if ( forbiddenHostNames().contains( m_hostname, Qt::CaseInsensitive ) )
    {
        return tr( "'%1' is not allowed as hostname." ).arg( m_hostname );
    }

    if ( m_hostname.indexOf( HOSTNAME_RX ) != 0 )
    {
        return tr( "Only letters, numbers, underscore and hyphen are allowed." );
    }

    return QString();
}

static QString
cleanupForHostname( const QString& s )
{
    QRegularExpression dmirx( "(^Apple|\\(.*\\)|[^a-zA-Z0-9])", QRegularExpression::CaseInsensitiveOption );
    return s.toLower().replace( dmirx, " " ).remove( ' ' );
}

/** @brief Guess the machine's name
 *
 * If there is DMI data, use that; otherwise, just call the machine "-pc".
 * Reads the DMI data just once.
 */
static QString
guessProductName()
{
    static bool tried = false;
    static QString dmiProduct;

    if ( !tried )
    {
        QFile dmiFile( QStringLiteral( "/sys/devices/virtual/dmi/id/product_name" ) );
        QFile modelFile( QStringLiteral( "/proc/device-tree/model" ) );

        if ( dmiFile.exists() && dmiFile.open( QIODevice::ReadOnly ) )
        {
            dmiProduct = cleanupForHostname( QString::fromLocal8Bit( dmiFile.readAll().simplified().data() ) );
            if ( !dmiProduct.isEmpty() )
            {
                tried = true;
                return dmiProduct;
            }
        }

        if ( modelFile.exists() && modelFile.open( QIODevice::ReadOnly ) )
        {
            dmiProduct
                = cleanupForHostname( QString::fromLocal8Bit( modelFile.readAll().chopped( 1 ).simplified().data() ) );
            if ( !dmiProduct.isEmpty() )
            {
                tried = true;
                return dmiProduct;
            }
        }

        dmiProduct = QStringLiteral( "pc" );
        tried = true;
    }
    return dmiProduct;
}
#ifdef HAVE_ICU
static QString
transliterate( const QString& input )
{
    static auto ue = UErrorCode::U_ZERO_ERROR;
    static auto transliterator = std::unique_ptr< icu::Transliterator >(
        icu::Transliterator::createInstance( TRANSLITERATOR_ID, UTRANS_FORWARD, ue ) );

    if ( ue != UErrorCode::U_ZERO_ERROR )
    {
        cWarning() << "Can't create transliterator";

        //it'll be checked later for non-ASCII characters
        return input;
    }

    icu::UnicodeString transliterable( input.utf16() );
    transliterator->transliterate( transliterable );
    return QString::fromUtf16( transliterable.getTerminatedBuffer() );
}
#else
static QString
transliterate( const QString& input )
{
    return input;
}
#endif

static QString
makeLoginNameSuggestion( const QStringList& parts )
{
    if ( parts.isEmpty() || parts.first().isEmpty() )
    {
        return QString();
    }

    QString usernameSuggestion = parts.first();
    for ( int i = 1; i < parts.length(); ++i )
    {
        if ( !parts.value( i ).isEmpty() )
        {
            usernameSuggestion.append( parts.value( i ).at( 0 ) );
        }
    }

    return usernameSuggestion.indexOf( USERNAME_RX ) != -1 ? usernameSuggestion : QString();
}

/** @brief Return an invalid string for use in a hostname, if @p s is empty
 *
 * Maps empty to "^" (which is invalid in a hostname), everything else
 * returns @p s itself.
 */
static QString
invalidEmpty( const QString& s )
{
    return s.isEmpty() ? QStringLiteral( "^" ) : s;
}

STATICTEST QString
makeHostnameSuggestion( const QString& templateString, const QStringList& fullNameParts, const QString& loginName )
{
    Calamares::String::DictionaryExpander d;
    // User data
    d.add( QStringLiteral( "first" ),
           invalidEmpty( fullNameParts.isEmpty() ? QString() : cleanupForHostname( fullNameParts.first() ) ) )
        .add( QStringLiteral( "name" ), invalidEmpty( cleanupForHostname( fullNameParts.join( QString() ) ) ) )
        .add( QStringLiteral( "login" ), invalidEmpty( cleanupForHostname( loginName ) ) )
        // Hardware data
        .add( QStringLiteral( "product" ), guessProductName() )
        .add( QStringLiteral( "product2" ), cleanupForHostname( QSysInfo::prettyProductName() ) )
        .add( QStringLiteral( "cpu" ), cleanupForHostname( QSysInfo::currentCpuArchitecture() ) )
        // Hostname data
        .add( QStringLiteral( "host" ), invalidEmpty( cleanupForHostname( QSysInfo::machineHostName() ) ) );

    QString hostnameSuggestion = d.expand( templateString );

    return hostnameSuggestion.indexOf( HOSTNAME_RX ) != -1 ? hostnameSuggestion : QString();
}

void
Config::setFullName( const QString& name )
{
    CONFIG_PREVENT_EDITING( QString, "fullName" );

    if ( name.isEmpty() && !m_fullName.isEmpty() )
    {
        if ( !m_customHostName )
        {
            setHostName( name );
        }
        if ( !m_customLoginName )
        {
            setLoginName( name );
        }
        m_fullName = name;
        emit fullNameChanged( name );
    }

    if ( name != m_fullName )
    {
        m_fullName = name;
        Calamares::GlobalStorage* gs = Calamares::JobQueue::instance()->globalStorage();
        if ( name.isEmpty() )
        {
            gs->remove( "fullname" );
        }
        else
        {
            gs->insert( "fullname", name );
        }
        emit fullNameChanged( name );

        // Build login and hostname, if needed
        static QRegularExpression rx( "[^a-zA-Z0-9 ]" );

        const QString cleanName = Calamares::String::removeDiacritics( transliterate( name ) )
                                      .replace( QRegularExpression( "[-']" ), "" )
                                      .replace( rx, " " )
                                      .toLower()
                                      .simplified();

        QStringList cleanParts = cleanName.split( ' ' );

        if ( !m_customLoginName )
        {
            const QString login = makeLoginNameSuggestion( cleanParts );
            if ( !login.isEmpty() && login != m_loginName )
            {
                setLoginName( login );
                // It's **still** not custom, though setLoginName() sets that
                m_customLoginName = false;
            }
        }
        if ( !m_customHostName )
        {
            const QString hostname = makeHostnameSuggestion( m_hostnameTemplate, cleanParts, loginName() );
            if ( !hostname.isEmpty() && hostname != m_hostname )
            {
                setHostName( hostname );
                // Still not custom
                m_customHostName = false;
            }
        }
    }
}

void
Config::setAutoLogin( bool b )
{
    if ( b != m_doAutoLogin )
    {
        m_doAutoLogin = b;
        updateGSAutoLogin( b, loginName() );
        emit autoLoginChanged( b );
    }
}

void
Config::setReuseUserPasswordForRoot( bool reuse )
{
    if ( reuse != m_reuseUserPasswordForRoot )
    {
        m_reuseUserPasswordForRoot = reuse;
        emit reuseUserPasswordForRootChanged( reuse );
        {
            auto rp = rootPasswordStatus();
            emit rootPasswordStatusChanged( rp.first, rp.second );
        }
    }
}

void
Config::setRequireStrongPasswords( bool strong )
{
    if ( strong != m_requireStrongPasswords )
    {
        m_requireStrongPasswords = strong;
        emit requireStrongPasswordsChanged( strong );
        {
            auto rp = rootPasswordStatus();
            emit rootPasswordStatusChanged( rp.first, rp.second );
        }
        {
            auto up = userPasswordStatus();
            emit userPasswordStatusChanged( up.first, up.second );
        }
    }
}

void
Config::setUserPassword( const QString& s )
{
    if ( s != m_userPassword )
    {
        m_userPassword = s;
        const auto p = passwordStatus( m_userPassword, m_userPasswordSecondary );
        emit userPasswordStatusChanged( p.first, p.second );
        emit userPasswordChanged( s );
    }
}

void
Config::setUserPasswordSecondary( const QString& s )
{
    if ( s != m_userPasswordSecondary )
    {
        m_userPasswordSecondary = s;
        const auto p = passwordStatus( m_userPassword, m_userPasswordSecondary );
        emit userPasswordStatusChanged( p.first, p.second );
        emit userPasswordSecondaryChanged( s );
    }
}

/** @brief Checks two copies of the password for validity
 *
 * Given two copies of the password -- generally the password and
 * the secondary fields -- checks them for validity and returns
 * a pair of <validity, message>.
 *
 */
Config::PasswordStatus
Config::passwordStatus( const QString& pw1, const QString& pw2 ) const
{
    if ( pw1 != pw2 )
    {
        return qMakePair( PasswordValidity::Invalid, tr( "Your passwords do not match!" ) );
    }

    bool failureIsFatal = requireStrongPasswords();
    for ( const auto& pc : m_passwordChecks )
    {
        QString message = pc.filter( pw1 );

        if ( !message.isEmpty() )
        {
            return qMakePair( failureIsFatal ? PasswordValidity::Invalid : PasswordValidity::Weak, message );
        }
    }

    return qMakePair( PasswordValidity::Valid, tr( "OK!" ) );
}

Config::PasswordStatus
Config::userPasswordStatus() const
{
    return passwordStatus( m_userPassword, m_userPasswordSecondary );
}

int
Config::userPasswordValidity() const
{
    auto p = userPasswordStatus();
    return p.first;
}

QString
Config::userPasswordMessage() const
{
    auto p = userPasswordStatus();
    return p.second;
}

void
Config::setRootPassword( const QString& s )
{
    if ( writeRootPassword() && s != m_rootPassword )
    {
        m_rootPassword = s;
        const auto p = passwordStatus( m_rootPassword, m_rootPasswordSecondary );
        emit rootPasswordStatusChanged( p.first, p.second );
        emit rootPasswordChanged( s );
    }
}

void
Config::setRootPasswordSecondary( const QString& s )
{
    if ( writeRootPassword() && s != m_rootPasswordSecondary )
    {
        m_rootPasswordSecondary = s;
        const auto p = passwordStatus( m_rootPassword, m_rootPasswordSecondary );
        emit rootPasswordStatusChanged( p.first, p.second );
        emit rootPasswordSecondaryChanged( s );
    }
}

void
Config::setActiveDirectoryUsed( bool used )
{
    m_activeDirectoryUsed = used;
}

bool
Config::getActiveDirectoryEnabled() const
{
    return m_activeDirectory;
}

bool
Config::getActiveDirectoryUsed() const
{
    return m_activeDirectoryUsed && m_activeDirectory;
}

void
Config::setActiveDirectoryAdminUsername( const QString& s )
{
    m_activeDirectoryAdminUsername = s;
}

void
Config::setActiveDirectoryAdminPassword( const QString& s )
{
    m_activeDirectoryAdminPassword = s;
}

void
Config::setActiveDirectoryDomain( const QString& s )
{
    m_activeDirectoryDomain = s;
}

void
Config::setActiveDirectoryIP( const QString& s )
{
    m_activeDirectoryIP = s;
}

QString
Config::rootPassword() const
{
    if ( writeRootPassword() )
    {
        if ( reuseUserPasswordForRoot() )
        {
            return userPassword();
        }
        return m_rootPassword;
    }
    return QString();
}

QString
Config::rootPasswordSecondary() const
{
    if ( writeRootPassword() )
    {
        if ( reuseUserPasswordForRoot() )
        {
            return userPasswordSecondary();
        }
        return m_rootPasswordSecondary;
    }
    return QString();
}

Config::PasswordStatus
Config::rootPasswordStatus() const
{
    if ( writeRootPassword() && !reuseUserPasswordForRoot() )
    {
        return passwordStatus( m_rootPassword, m_rootPasswordSecondary );
    }
    else
    {
        return userPasswordStatus();
    }
}

int
Config::rootPasswordValidity() const
{
    auto p = rootPasswordStatus();
    return p.first;
}

QString
Config::rootPasswordMessage() const
{
    auto p = rootPasswordStatus();
    return p.second;
}

bool
Config::isReady() const
{
    bool readyFullName = !fullName().isEmpty();  // Needs some text
    bool readyHostname = hostnameStatus().isEmpty();  // .. no warning message
    bool readyUsername = !loginName().isEmpty() && loginNameStatus().isEmpty();  // .. no warning message
    bool readyUserPassword = userPasswordValidity() != Config::PasswordValidity::Invalid;
    bool readyRootPassword = rootPasswordValidity() != Config::PasswordValidity::Invalid;
    return readyFullName && readyHostname && readyUsername && readyUserPassword && readyRootPassword;
}

/** @brief Update ready status and emit signal
 *
 * This is a "concentrator" private slot for all the status-changed
 * signals, so that readyChanged() is emitted only when needed.
 */
void
Config::checkReady()
{
    bool b = isReady();
    if ( b != m_isReady )
    {
        m_isReady = b;
        emit readyChanged( b );
    }
}

STATICTEST void
setConfigurationDefaultGroups( const QVariantMap& map, QList< GroupDescription >& defaultGroups )
{
    defaultGroups.clear();

    const QString key( "defaultGroups" );
    auto groupsFromConfig = map.value( key ).toList();
    if ( groupsFromConfig.isEmpty() )
    {
        if ( map.contains( key ) && map.value( key ).isValid() && map.value( key ).canConvert< QVariantList >() )
        {
            // Explicitly set, but empty: this is valid, but unusual.
            cDebug() << key << "has explicit empty value.";
        }
        else
        {
            // By default give the user a handful of "traditional" groups, if
            // none are specified at all. These are system (GID < 1000) groups.
            cWarning() << "Using fallback groups. Please check *defaultGroups* value in users.conf";
            for ( const auto& s : { "lp", "video", "network", "storage", "wheel", "audio" } )
            {
                defaultGroups.append(
                    GroupDescription( s, GroupDescription::CreateIfNeeded {}, GroupDescription::SystemGroup {} ) );
            }
        }
    }
    else
    {
        for ( const auto& v : groupsFromConfig )
        {
            if ( Calamares::typeOf( v ) == Calamares::StringVariantType )
            {
                defaultGroups.append( GroupDescription( v.toString() ) );
            }
            else if ( Calamares::typeOf( v ) == Calamares::MapVariantType )
            {
                const auto innermap = v.toMap();
                QString name = Calamares::getString( innermap, "name" );
                if ( !name.isEmpty() )
                {
                    defaultGroups.append( GroupDescription( name,
                                                            Calamares::getBool( innermap, "must_exist", false ),
                                                            Calamares::getBool( innermap, "system", false ) ) );
                }
                else
                {
                    cWarning() << "Ignoring *defaultGroups* entry without a name" << v;
                }
            }
            else
            {
                cWarning() << "Unknown *defaultGroups* entry" << v;
            }
        }
    }
}

STATICTEST HostNameAction
getHostNameAction( const QVariantMap& configurationMap )
{
    HostNameAction setHostName = HostNameAction::EtcHostname;
    QString hostnameActionString = Calamares::getString( configurationMap, "location" );
    if ( !hostnameActionString.isEmpty() )
    {
        bool ok = false;
        setHostName = hostnameActionNames().find( hostnameActionString, ok );
        if ( !ok )
        {
            setHostName = HostNameAction::EtcHostname;  // Rather than none
        }
    }

    return setHostName;
}

/** @brief Process entries in the passwordRequirements config entry
 *
 * Called once for each item in the config entry, which should
 * be a key-value pair. What makes sense as a value depends on
 * the key. Supported keys are documented in users.conf.
 *
 * @return if the check was added, returns @c true
 */
STATICTEST bool
addPasswordCheck( const QString& key, const QVariant& value, PasswordCheckList& passwordChecks )
{
    if ( key == "minLength" )
    {
        add_check_minLength( passwordChecks, value );
    }
    else if ( key == "maxLength" )
    {
        add_check_maxLength( passwordChecks, value );
    }
    else if ( key == "nonempty" )
    {
        cWarning() << "nonempty check is ignored; use minLength: 1";
        return false;
    }
#ifdef CHECK_PWQUALITY
    else if ( key == "libpwquality" )
    {
        add_check_libpwquality( passwordChecks, value );
    }
#endif  // CHECK_PWQUALITY
    else
    {
        cWarning() << "Unknown password-check key" << key;
        return false;
    }
    return true;
}

/** @brief Returns a value of either key from the map
 *
 * Takes a function (e.g. getBool, or getString) and two keys,
 * returning the value in the map of the one that is there (or @p defaultArg)
 */
template < typename T, typename U >
T
either( T ( *f )( const QVariantMap&, const QString&, U ),
        const QVariantMap& configurationMap,
        const QString& oldKey,
        const QString& newKey,
        U defaultArg )
{
    if ( configurationMap.contains( oldKey ) )
    {
        return f( configurationMap, oldKey, defaultArg );
    }
    else
    {
        return f( configurationMap, newKey, defaultArg );
    }
}

/** @brief Tidy up a list of names
 *
 * Remove duplicates, apply lowercase, sort.
 */
static void
tidy( QStringList& l )
{
    std::for_each( l.begin(), l.end(), []( QString& s ) { s = s.toLower(); } );
    l.sort();
    l.removeDuplicates();
}

static QString
unscrambleYAML( const QVariant& v )
{
    if ( Calamares::isIntegerVariantType( v ) )
    {
        // YAML takes a string like "0755" and makes it an integer **anyway**
        const auto number = v.toLongLong();
        if ( number < 0 )
        {
            return QString();
        }
        // Since YAML has parsed it as a decimal number,
        // turn it back into the string representation of
        // that decimal number, even though we intended it
        // to be octal (e.g. "755" written down becomes
        // seven-hundred-fifty-five, needs to be the string
        // "755" again, even though we meant octal 755 which
        // is four-hundred-ninety-three.
        if ( number > 777 ) { return QString(); }
        return QString::number( number );
    }
    return v.toString();
}

void
Config::setConfigurationMap( const QVariantMap& configurationMap )
{
    // Handle *user* key and subkeys and legacy settings
    {
        bool ok = false;  // Ignored
        QVariantMap userSettings = Calamares::getSubMap( configurationMap, "user", ok );

        QString shell( QLatin1String( "/bin/bash" ) );  // as if it's not set at all
        if ( userSettings.contains( "shell" ) )
        {
            shell = Calamares::getString( userSettings, "shell" );
        }
        // Now it might be explicitly set to empty, which is ok
        setUserShell( shell );

        m_forbiddenLoginNames = Calamares::getStringList( userSettings, "forbidden_names" );
        m_forbiddenLoginNames << alwaysForbiddenLoginNames();
        tidy( m_forbiddenLoginNames );

        const auto permissionKey = QStringLiteral( "home_permissions" );
        if ( userSettings.contains( permissionKey ) )
        {
            const auto value = unscrambleYAML( userSettings.value( permissionKey ) );
            m_homeDirPermissions = Calamares::parseFileMode( value );
            if ( m_homeDirPermissions < 0 )
            {
                cWarning() << "Setting for" << permissionKey << '(' << value << userSettings[ permissionKey ]
                           << ") is invalid.";
            }
        }
        else
        {
            m_homeDirPermissions = -1;
        }
    }

    setAutoLoginGroup( either< QString, const QString& >(
        Calamares::getString, configurationMap, "autologinGroup", "autoLoginGroup", QString() ) );
    setSudoersGroup( Calamares::getString( configurationMap, "sudoersGroup" ) );
    m_sudoStyle = Calamares::getBool( configurationMap, "sudoersConfigureWithGroup", false ) ? SudoStyle::UserAndGroup
                                                                                             : SudoStyle::UserOnly;

    // Handle Active Directory enablement
    m_activeDirectory = Calamares::getBool( configurationMap, "allowActiveDirectory", false );

    // Handle *hostname* key and subkeys and legacy settings
    {
        bool ok = false;  // Ignored
        QVariantMap hostnameSettings = Calamares::getSubMap( configurationMap, "hostname", ok );

        m_hostnameAction = getHostNameAction( hostnameSettings );
        m_writeEtcHosts = Calamares::getBool( hostnameSettings, "writeHostsFile", true );
        m_hostnameTemplate
            = Calamares::getString( hostnameSettings, "template", QStringLiteral( "${first}-${product}" ) );

        m_forbiddenHostNames = Calamares::getStringList( hostnameSettings, "forbidden_names" );
        m_forbiddenHostNames << alwaysForbiddenHostNames();
        tidy( m_forbiddenHostNames );
    }

    setConfigurationDefaultGroups( configurationMap, m_defaultGroups );

    // Renaming of Autologin -> AutoLogin in 4ffa79d4cf also affected
    // configuration keys, which was not intended. Accept both.
    m_displayAutoLogin = Calamares::getBool( configurationMap, "displayAutologin", false );
    m_doAutoLogin = either(
        Calamares::getBool, configurationMap, QStringLiteral( "doAutologin" ), QStringLiteral( "doAutoLogin" ), false );

    m_writeRootPassword = Calamares::getBool( configurationMap, "setRootPassword", true );
    Calamares::JobQueue::instance()->globalStorage()->insert( "setRootPassword", m_writeRootPassword );

    m_reuseUserPasswordForRoot = Calamares::getBool( configurationMap, "doReusePassword", false );

    m_permitWeakPasswords = Calamares::getBool( configurationMap, "allowWeakPasswords", false );
    m_requireStrongPasswords
        = !m_permitWeakPasswords || !Calamares::getBool( configurationMap, "allowWeakPasswordsDefault", false );

    // If the value doesn't exist, or isn't a map, this gives an empty map -- no problem
    auto pr_checks( configurationMap.value( "passwordRequirements" ).toMap() );
    for ( decltype( pr_checks )::const_iterator i = pr_checks.constBegin(); i != pr_checks.constEnd(); ++i )
    {
        addPasswordCheck( i.key(), i.value(), m_passwordChecks );
    }
    std::sort( m_passwordChecks.begin(), m_passwordChecks.end() );

    updateGSAutoLogin( doAutoLogin(), loginName() );
    checkReady();

    ApplyPresets( *this, configurationMap ) << "fullName"
                                            << "loginName";
}

void
Config::finalizeGlobalStorage() const
{
    updateGSAutoLogin( doAutoLogin(), loginName() );

    Calamares::GlobalStorage* gs = Calamares::JobQueue::instance()->globalStorage();
    if ( writeRootPassword() )
    {
        gs->insert( "reuseRootPassword", reuseUserPasswordForRoot() );
    }
    gs->insert( "password", Calamares::String::obscure( userPassword() ) );
}

Calamares::JobList
Config::createJobs() const
{
    Calamares::JobList jobs;

    if ( !isReady() )
    {
        return jobs;
    }

    Calamares::Job* j;

    if ( !m_sudoersGroup.isEmpty() )
    {
        j = new SetupSudoJob( m_sudoersGroup, m_sudoStyle );
        jobs.append( Calamares::job_ptr( j ) );
    }

    if ( getActiveDirectoryUsed() )
    {
        j = new ActiveDirectoryJob( m_activeDirectoryAdminUsername,
                                    m_activeDirectoryAdminPassword,
                                    m_activeDirectoryDomain,
                                    m_activeDirectoryIP );
        jobs.append( Calamares::job_ptr( j ) );
    }

    j = new SetupGroupsJob( this );
    jobs.append( Calamares::job_ptr( j ) );

    j = new CreateUserJob( this );
    jobs.append( Calamares::job_ptr( j ) );

    j = new SetPasswordJob( loginName(), userPassword() );
    jobs.append( Calamares::job_ptr( j ) );

    j = new SetPasswordJob( "root", rootPassword() );
    jobs.append( Calamares::job_ptr( j ) );

    j = new SetHostNameJob( this );
    jobs.append( Calamares::job_ptr( j ) );

    return jobs;
}
