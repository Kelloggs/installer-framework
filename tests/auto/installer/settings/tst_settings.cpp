#include "settings.h"
#include "errors.h"
#include "repository.h"

#include <QFile>
#include <QString>
#include <QTest>

using namespace QInstaller;

class tst_Settings : public QObject
{
    Q_OBJECT

private slots:
    void loadTutorialConfig();
    void loadFullConfig();
    void loadEmptyConfig();
    void loadNotExistingConfig();
    void loadMalformedConfig();
    void loadUnknownElementConfigInStrictParseMode();
    void loadUnknownElementConfigInRelaxedParseMode();
    void loadMinimalConfigTagDefaults();
    void loadUnexpectedAttributeConfig();
    void loadUnexpectedTagConfig();
};

void tst_Settings::loadTutorialConfig()
{
    Settings settings = Settings::fromFileAndPrefix(":///data/tutorial_config.xml", ":///data");

    // specified values
    QCOMPARE(settings.applicationName(), QLatin1String("Your application"));
    QCOMPARE(settings.version(), QLatin1String("1.2.3"));
    QCOMPARE(settings.title(), QLatin1String("Your application Installer"));
    QCOMPARE(settings.publisher(), QLatin1String("Your vendor"));
    QCOMPARE(settings.startMenuDir(), QLatin1String("Super App"));
    QCOMPARE(settings.targetDir(), QLatin1String("@RootDir@InstallationDirectory"));

    // default values
    QCOMPARE(settings.logo(), QString());
    QCOMPARE(settings.url(), QString());
    QCOMPARE(settings.watermark(), QString());
    QCOMPARE(settings.banner(), QString());
    QCOMPARE(settings.background(), QString());
#if defined(Q_OS_WIN)
    QCOMPARE(settings.installerApplicationIcon(), QLatin1String(":/installer.ico"));
    QCOMPARE(settings.installerWindowIcon(), QLatin1String(":/installer.ico"));
    QCOMPARE(settings.systemIconSuffix(), QLatin1String(".ico"));
#elif defined(Q_OS_OSX)
    QCOMPARE(settings.installerApplicationIcon(), QLatin1String(":/installer.icns"));
    QCOMPARE(settings.installerWindowIcon(), QLatin1String(":/installer.icns"));
    QCOMPARE(settings.systemIconSuffix(), QLatin1String(".icns"));
#else
    QCOMPARE(settings.installerApplicationIcon(), QLatin1String(":/installer.png"));
    QCOMPARE(settings.installerWindowIcon(), QLatin1String(":/installer.png"));
    QCOMPARE(settings.systemIconSuffix(), QLatin1String(".png"));
#endif
    QCOMPARE(settings.wizardStyle(), QString());
    QCOMPARE(settings.wizardDefaultWidth(), 0);
    QCOMPARE(settings.wizardDefaultHeight(), 0);
    QCOMPARE(settings.titleColor(), QString());
    QCOMPARE(settings.runProgram(), QString());
    QCOMPARE(settings.runProgramArguments(), QStringList());
    QCOMPARE(settings.runProgramDescription(), QString());
    QCOMPARE(settings.adminTargetDir(), QString());
    QCOMPARE(settings.removeTargetDir(), QLatin1String("true"));
    QCOMPARE(settings.maintenanceToolName(), QLatin1String("maintenancetool"));
    QCOMPARE(settings.maintenanceToolIniFile(), QLatin1String("maintenancetool.ini"));
    QCOMPARE(settings.configurationFileName(), QLatin1String("components.xml"));
    QCOMPARE(settings.dependsOnLocalInstallerBinary(), false);
    QCOMPARE(settings.repositorySettingsPageVisible(), true);
    QCOMPARE(settings.allowSpaceInPath(), true);
    QCOMPARE(settings.allowNonAsciiCharacters(), false);
    QCOMPARE(settings.createLocalRepository(), false);
    QCOMPARE(settings.installActionColumnVisible(), false);

    QCOMPARE(settings.hasReplacementRepos(), false);
    QCOMPARE(settings.repositories(), QSet<Repository>());
    QCOMPARE(settings.defaultRepositories(), QSet<Repository>());
    QCOMPARE(settings.temporaryRepositories(), QSet<Repository>());
    QCOMPARE(settings.userRepositories(), QSet<Repository>());

    QCOMPARE(settings.proxyType(), Settings::NoProxy);
    QCOMPARE(settings.ftpProxy(), QNetworkProxy());
    QCOMPARE(settings.httpProxy(), QNetworkProxy());

    QCOMPARE(settings.translations(), QStringList());
    QCOMPARE(settings.controlScript(), QString());
}

void tst_Settings::loadFullConfig()
{
    Settings settings = Settings::fromFileAndPrefix(":///data/full_config.xml", ":///data");
}

void tst_Settings::loadEmptyConfig()
{
    QTest::ignoreMessage(QtDebugMsg, "create Error-Exception: \"Missing or empty <Name> tag in "
                         ":/data/empty_config.xml.\" ");
    try {
        Settings::fromFileAndPrefix(":/data/empty_config.xml", ":/data");
    } catch (const Error &error) {
        QCOMPARE(error.message(), QLatin1String("Missing or empty <Name> tag in :/data/empty_config.xml."));
        return;
    }
    QFAIL("No exception thrown");
}

void tst_Settings::loadNotExistingConfig()
{
    QTest::ignoreMessage(QtDebugMsg, "create Error-Exception: \"Could not open settings file "
                         ":/data/inexisting_config.xml for reading: "
                         "Unknown error\" ");
    try {
        Settings::fromFileAndPrefix(":/data/inexisting_config.xml", ":/data");
    } catch (const Error &error) {
        QCOMPARE(error.message(), QLatin1String("Could not open settings file "
                                                ":/data/inexisting_config.xml for reading: "
                                                "Unknown error"));
        return;
    }
    QFAIL("No exception thrown");
}

void tst_Settings::loadMalformedConfig()
{
    QTest::ignoreMessage(QtDebugMsg, "create Error-Exception: \"Error in :/data/malformed_config.xml, "
                         "line 9, column 0: Premature end of document.\" ");
    try {
        Settings::fromFileAndPrefix(":/data/malformed_config.xml", ":/data");
    } catch (const Error &error) {
        QCOMPARE(error.message(), QLatin1String("Error in :/data/malformed_config.xml, line 9, column 0: "
                                           "Premature end of document."));
        return;
    }
    QFAIL("No exception thrown");
}

void tst_Settings::loadUnknownElementConfigInStrictParseMode()
{
    QTest::ignoreMessage(QtDebugMsg, "create Error-Exception: \"Error in :/data/unknown_element_config.xml, "
        "line 5, column 13: Unexpected element 'unknown'.\" ");
    try {
        Settings::fromFileAndPrefix(":/data/unknown_element_config.xml", ":/data");
    } catch (const Error &error) {
        QCOMPARE(error.message(), QLatin1String("Error in :/data/unknown_element_config.xml, line 5, "
                                                "column 13: Unexpected element 'unknown'."));
        return;
    }
    QFAIL("No exception thrown");
}

void tst_Settings::loadUnknownElementConfigInRelaxedParseMode()
{
    QTest::ignoreMessage(QtWarningMsg, "\"Ignoring following settings reader error in "
        ":/data/unknown_element_config.xml, line 5, column 13: Unexpected element 'unknown'.\" ");
    try {
        Settings settings = Settings::fromFileAndPrefix(":/data/unknown_element_config.xml", ":/data",
            Settings::RelaxedParseMode);
        QCOMPARE(settings.title(), QLatin1String("Your application Installer"));
    } catch (const Error &error) {
        QFAIL(qPrintable(QString::fromLatin1("Got an exception in TolerantParseMode: %1").arg(error.message())));
    }
}

void tst_Settings::loadMinimalConfigTagDefaults()
{
    Settings settings = Settings::fromFileAndPrefix(":///data/minimal_config_tag_defaults.xml",
        ":///data");

    // These tags are not mandatory, though need to be set to default values.
    QCOMPARE(settings.configurationFileName(), QLatin1String("components.xml"));

    QCOMPARE(settings.maintenanceToolName(), QLatin1String("maintenancetool"));
    QCOMPARE(settings.maintenanceToolIniFile(), QLatin1String("maintenancetool.ini"));
}

void tst_Settings::loadUnexpectedAttributeConfig()
{
    QTest::ignoreMessage(QtDebugMsg, "create Error-Exception: \"Error in "
        ":///data/unexpectedattribute_config.xml, line 6, column 27: Unexpected attribute "
        "for element 'Argument'.\" ");

    try {
        Settings::fromFileAndPrefix(":///data/unexpectedattribute_config.xml", ":///data");
    } catch (const Error &error) {
        QCOMPARE(error.message(), QLatin1String("Error in :///data/unexpectedattribute_config.xml,"
           " line 6, column 27: Unexpected attribute for element 'Argument'."));
        return;
    }
    QFAIL("No exception thrown");

    return;
}

void tst_Settings::loadUnexpectedTagConfig()
{
    QTest::ignoreMessage(QtDebugMsg, "create Error-Exception: \"Error in "
        ":///data/unexpectedtag_config.xml, line 6, column 12: Unexpected element 'Foo'.\" ");

    try {
        Settings::fromFileAndPrefix(":///data/unexpectedtag_config.xml", ":///data");
    } catch (const Error &error) {
        QCOMPARE(error.message(), QLatin1String("Error in :///data/unexpectedtag_config.xml,"
           " line 6, column 12: Unexpected element 'Foo'."));
        return;
    }
    QFAIL("No exception thrown");

    return;
}


QTEST_MAIN(tst_Settings)

#include "tst_settings.moc"
