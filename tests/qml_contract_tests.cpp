#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextStream>

#ifndef WIMFORGE_SOURCE_DIR
#error WIMFORGE_SOURCE_DIR must point at the WimForge source tree
#endif

namespace {

class TestContext
{
public:
    void check(bool condition, const QString &message)
    {
        if (condition)
            return;
        ++m_failures;
        QTextStream(stderr) << "FAIL: " << message << '\n';
    }

    [[nodiscard]] int failures() const { return m_failures; }

private:
    int m_failures = 0;
};

QString readText(const QString &path, TestContext *test)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        test->check(false, QStringLiteral("Could not read %1: %2").arg(path, file.errorString()));
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

QSet<QString> captures(const QString &text, const QRegularExpression &expression)
{
    QSet<QString> values;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext())
        values.insert(matches.next().captured(1));
    return values;
}

QStringList captureList(const QString &text, const QRegularExpression &expression)
{
    QStringList values;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext())
        values.append(matches.next().captured(1));
    return values;
}

QString withoutComments(const QString &text)
{
    QString result;
    result.reserve(text.size());
    QChar quote;
    bool escaped = false;
    bool lineComment = false;
    bool blockComment = false;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar current = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar{};
        if (lineComment) {
            if (current == QLatin1Char('\n')) {
                lineComment = false;
                result.append(current);
            }
            continue;
        }
        if (blockComment) {
            if (current == QLatin1Char('*') && next == QLatin1Char('/')) {
                blockComment = false;
                ++index;
            } else if (current == QLatin1Char('\n')) {
                result.append(current);
            }
            continue;
        }
        if (!quote.isNull()) {
            result.append(current);
            if (escaped) {
                escaped = false;
            } else if (current == QLatin1Char('\\')) {
                escaped = true;
            } else if (current == quote) {
                quote = {};
            }
            continue;
        }
        if (current == QLatin1Char('/') && next == QLatin1Char('/')) {
            lineComment = true;
            ++index;
        } else if (current == QLatin1Char('/') && next == QLatin1Char('*')) {
            blockComment = true;
            ++index;
        } else {
            result.append(current);
            if (current == QLatin1Char('"') || current == QLatin1Char('\''))
                quote = current;
        }
    }
    return result;
}

QString joinedSorted(const QSet<QString> &values)
{
    QStringList sorted = values.values();
    sorted.sort();
    return sorted.join(QStringLiteral(", "));
}

qsizetype matchCount(const QString &text, const QRegularExpression &expression)
{
    qsizetype count = 0;
    auto matches = expression.globalMatch(text);
    while (matches.hasNext()) {
        matches.next();
        ++count;
    }
    return count;
}

bool hasReadonlyPropertyValue(const QString &text, const QString &type,
                              const QString &name, const QString &value)
{
    const QRegularExpression expression(
        QStringLiteral("\\breadonly\\s+property\\s+%1\\s+%2\\s*:\\s*%3(?![A-Za-z0-9_.])")
            .arg(QRegularExpression::escape(type),
                 QRegularExpression::escape(name),
                 QRegularExpression::escape(value)));
    return expression.match(text).hasMatch();
}

QString lowerFirst(QString value)
{
    if (!value.isEmpty())
        value[0] = value.at(0).toLower();
    return value;
}

qsizetype matchingDelimiter(const QString &text, qsizetype openingDelimiter,
                            QChar open, QChar close)
{
    int depth = 0;
    QChar quote;
    bool escaped = false;
    bool lineComment = false;
    bool blockComment = false;
    for (qsizetype index = openingDelimiter; index < text.size(); ++index) {
        const QChar current = text.at(index);
        const QChar next = index + 1 < text.size() ? text.at(index + 1) : QChar{};
        if (lineComment) {
            if (current == QLatin1Char('\n'))
                lineComment = false;
            continue;
        }
        if (blockComment) {
            if (current == QLatin1Char('*') && next == QLatin1Char('/')) {
                blockComment = false;
                ++index;
            }
            continue;
        }
        if (!quote.isNull()) {
            if (escaped) {
                escaped = false;
            } else if (current == QLatin1Char('\\')) {
                escaped = true;
            } else if (current == quote) {
                quote = {};
            }
            continue;
        }
        if (current == QLatin1Char('/') && next == QLatin1Char('/')) {
            lineComment = true;
            ++index;
        } else if (current == QLatin1Char('/') && next == QLatin1Char('*')) {
            blockComment = true;
            ++index;
        } else if (current == QLatin1Char('"') || current == QLatin1Char('\'')) {
            quote = current;
        } else if (current == open) {
            ++depth;
        } else if (current == close) {
            --depth;
            if (depth == 0)
                return index;
        }
    }
    return -1;
}

qsizetype matchingBrace(const QString &text, qsizetype openingBrace)
{
    return matchingDelimiter(text, openingBrace, QLatin1Char('{'), QLatin1Char('}'));
}

QString arrayPropertyBlock(const QString &text, const QString &propertyName)
{
    const QRegularExpression propertyExpression(
        QStringLiteral("\\b(?:readonly\\s+)?property\\s+var\\s+%1\\s*:\\s*\\[")
            .arg(QRegularExpression::escape(propertyName)));
    const QRegularExpressionMatch property = propertyExpression.match(text);
    if (!property.hasMatch())
        return {};
    const qsizetype openingBracket = text.indexOf(QLatin1Char('['),
                                                   property.capturedStart());
    const qsizetype closingBracket = matchingDelimiter(
        text, openingBracket, QLatin1Char('['), QLatin1Char(']'));
    if (openingBracket < 0 || closingBracket < 0)
        return {};
    return text.mid(openingBracket + 1, closingBracket - openingBracket - 1);
}

bool hasLeadingSymbolTitle(const QString &text)
{
    const QRegularExpression titleExpression(
        QStringLiteral(R"re(\btitle\s*:\s*"([^"\r\n]*)")re"));
    auto matches = titleExpression.globalMatch(text);
    while (matches.hasNext()) {
        const QString title = matches.next().captured(1).trimmed();
        if (title.isEmpty())
            continue;
        if (title.startsWith(QStringLiteral("\\u"), Qt::CaseInsensitive))
            return true;
        const QChar first = title.front();
        const QChar::Category category = first.category();
        if (first.isHighSurrogate()
            || category == QChar::Symbol_Math
            || category == QChar::Symbol_Currency
            || category == QChar::Symbol_Modifier
            || category == QChar::Symbol_Other) {
            return true;
        }
    }
    return false;
}

QSet<QString> appConnectionHandlers(const QString &text,
                                    const QRegularExpression &handlerExpression)
{
    QSet<QString> handlers;
    const QRegularExpression connectionsExpression(
        QStringLiteral(R"(\bConnections\s*\{)"));
    const QRegularExpression appTargetExpression(
        QStringLiteral(R"(\btarget\s*:\s*(?:root\.)?(?:app|controller)\b)"));
    qsizetype offset = 0;
    while (offset < text.size()) {
        const QRegularExpressionMatch connection = connectionsExpression.match(text, offset);
        if (!connection.hasMatch())
            break;
        const qsizetype openingBrace = text.indexOf(QLatin1Char('{'),
                                                     connection.capturedStart());
        const qsizetype closingBrace = matchingBrace(text, openingBrace);
        if (openingBrace < 0 || closingBrace < 0)
            break;
        const QString block = text.mid(openingBrace + 1,
                                       closingBrace - openingBrace - 1);
        if (appTargetExpression.match(block).hasMatch())
            handlers.unite(captures(block, handlerExpression));
        offset = closingBrace + 1;
    }
    return handlers;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    TestContext test;
    const QString sourceRoot = QString::fromUtf8(WIMFORGE_SOURCE_DIR);
    const QString controllerHeader = readText(
        sourceRoot + QStringLiteral("/src/AppController.h"), &test);

    const QRegularExpression propertyExpression(
        QStringLiteral(R"(Q_PROPERTY\([^\n\)]*\b([A-Za-z_][A-Za-z0-9_]*)\s+READ\b)"));
    const QRegularExpression writablePropertyExpression(
        QStringLiteral(R"(Q_PROPERTY\([^\n\)]*\b([A-Za-z_][A-Za-z0-9_]*)\s+READ\b[^\n\)]*\bWRITE\b)"));
    const QRegularExpression invokableExpression(
        QStringLiteral(R"(Q_INVOKABLE\s+[^;\n\(]*\b([A-Za-z_][A-Za-z0-9_]*)\s*\()"));

    QSet<QString> readableApi = captures(controllerHeader, propertyExpression);
    const QSet<QString> writableApi = captures(controllerHeader, writablePropertyExpression);
    readableApi.unite(captures(controllerHeader, invokableExpression));

    const qsizetype signalsStart = controllerHeader.indexOf(QStringLiteral("signals:"));
    const qsizetype signalsEnd = controllerHeader.indexOf(QStringLiteral("private:"), signalsStart);
    test.check(signalsStart >= 0 && signalsEnd > signalsStart,
               QStringLiteral("AppController signals section could not be parsed"));
    const QString signalsSection = signalsStart >= 0 && signalsEnd > signalsStart
        ? controllerHeader.mid(signalsStart, signalsEnd - signalsStart)
        : QString();
    const QRegularExpression signalExpression(
        QStringLiteral(R"(^\s*void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\()"),
        QRegularExpression::MultilineOption);
    const QSet<QString> controllerSignals = captures(signalsSection, signalExpression);

    const QRegularExpression appReferenceExpression(
        QStringLiteral(R"(\bapp\.([A-Za-z_][A-Za-z0-9_]*))"));
    const QRegularExpression appAssignmentExpression(
        QStringLiteral(R"(\bapp\.([A-Za-z_][A-Za-z0-9_]*)\s*=(?!=))"));
    const QRegularExpression signalHandlerExpression(
        QStringLiteral(R"(\bfunction\s+on([A-Z][A-Za-z0-9_]*)\s*\()"));
    const QRegularExpression legacyMaterialPurpleExpression(
        QStringLiteral(R"((#(?:141218|1D1B20|211F26|2B292F|343139|49454F|4A4458|625B71|6750A4|79747E|938F99|CAC4D0|E7E0EC|E8DEF8|EDE7F1|F3EDF7|F7F2FA|FFFBFE))\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression invalidColumnStretchExpression(
        QStringLiteral(R"(\bcolumnStretchFactor\s*:)"));
    const QRegularExpression stateGlyphCallExpression(
        QStringLiteral(R"(\bstateGlyph\s*\()"));
    const QRegularExpression reservedForegroundPropertyExpression(
        QStringLiteral(R"(\b(?:readonly\s+)?property\s+color\s+on[A-Z][A-Za-z0-9_]*\s*:)"));

    int qmlFileCount = 0;
    QDirIterator iterator(sourceRoot + QStringLiteral("/qml"),
                          {QStringLiteral("*.qml")}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString text = readText(path, &test);
        const QString policyText = withoutComments(text);
        const QString relativePath = QDir(sourceRoot).relativeFilePath(path);
        ++qmlFileCount;

        for (const QString &name : captures(text, appReferenceExpression)) {
            test.check(readableApi.contains(name),
                       QStringLiteral("%1 references unknown AppController member app.%2")
                           .arg(relativePath, name));
        }
        for (const QString &name : captures(text, appAssignmentExpression)) {
            test.check(writableApi.contains(name),
                       QStringLiteral("%1 assigns read-only or unknown AppController property app.%2")
                           .arg(relativePath, name));
        }
        for (const QString &handler : appConnectionHandlers(text, signalHandlerExpression)) {
            const QString signal = lowerFirst(handler);
            test.check(controllerSignals.contains(signal),
                       QStringLiteral("%1 handles unknown AppController signal %2")
                           .arg(relativePath, signal));
        }

        const QRegularExpressionMatch legacyColor =
            legacyMaterialPurpleExpression.match(policyText);
        test.check(!legacyColor.hasMatch(),
                   QStringLiteral("%1 retains legacy Material-purple literal %2; use DesignTokens")
                       .arg(relativePath,
                            legacyColor.hasMatch() ? legacyColor.captured(1) : QString()));
        test.check(!invalidColumnStretchExpression.match(policyText).hasMatch(),
                   QStringLiteral("%1 uses invalid columnStretchFactor; use supported Layout sizing")
                       .arg(relativePath));
        test.check(!stateGlyphCallExpression.match(policyText).hasMatch(),
                   QStringLiteral("%1 calls removed stateGlyph(); render semantic status text/chips instead")
                       .arg(relativePath));
        test.check(!reservedForegroundPropertyExpression.match(policyText).hasMatch(),
                   QStringLiteral("%1 declares a color property with the reserved onX handler prefix; use a foreground name")
                       .arg(relativePath));
    }

    test.check(qmlFileCount >= 15,
               QStringLiteral("Expected to scan the complete QML module, found only %1 files")
                   .arg(qmlFileCount));

    const QString cmakeLists = readText(sourceRoot + QStringLiteral("/CMakeLists.txt"), &test);
    const QStringList materialComponents{
        QStringLiteral("DesignTokens.qml"),
        QStringLiteral("WfButton.qml"),
        QStringLiteral("WfCard.qml"),
        QStringLiteral("WfField.qml"),
        QStringLiteral("WfIconButton.qml"),
        QStringLiteral("WfPageHeader.qml"),
        QStringLiteral("WfStatusChip.qml"),
        QStringLiteral("ProjectStartPage.qml")
    };
    for (const QString &component : materialComponents) {
        const QString relativePath = QStringLiteral("qml/components/") + component;
        const QFileInfo componentFile(sourceRoot + QLatin1Char('/') + relativePath);
        test.check(componentFile.isFile() && componentFile.size() > 0,
                   QStringLiteral("Material component %1 must exist and be non-empty")
                       .arg(relativePath));
        test.check(cmakeLists.contains(relativePath),
                   QStringLiteral("Material component %1 must be registered in the QML module")
                       .arg(relativePath));
    }

    const QString designTokens = withoutComments(readText(
        sourceRoot + QStringLiteral("/qml/components/DesignTokens.qml"), &test));
    test.check(QRegularExpression(QStringLiteral(R"(\bpragma\s+Singleton\b)"))
                   .match(designTokens).hasMatch(),
               QStringLiteral("DesignTokens must remain a QML singleton"));
    const QList<QStringList> expectedTokens{
        {QStringLiteral("string"), QStringLiteral("fontDisplay"),
         QStringLiteral("\"Segoe UI Variable Display\"")},
        {QStringLiteral("string"), QStringLiteral("fontBody"),
         QStringLiteral("\"Segoe UI Variable Text\"")},
        {QStringLiteral("string"), QStringLiteral("fontMono"),
         QStringLiteral("\"Cascadia Mono\"")},
        {QStringLiteral("int"), QStringLiteral("radiusControl"), QStringLiteral("12")},
        {QStringLiteral("int"), QStringLiteral("radiusCard"), QStringLiteral("18")},
        {QStringLiteral("int"), QStringLiteral("radiusPill"), QStringLiteral("22")},
        {QStringLiteral("int"), QStringLiteral("navWidth"), QStringLiteral("260")},
        {QStringLiteral("int"), QStringLiteral("navCompactWidth"), QStringLiteral("76")},
        {QStringLiteral("int"), QStringLiteral("topBarHeight"), QStringLiteral("64")},
        {QStringLiteral("int"), QStringLiteral("controlHeight"), QStringLiteral("40")},
        {QStringLiteral("int"), QStringLiteral("fieldHeight"), QStringLiteral("44")},
        {QStringLiteral("int"), QStringLiteral("rowHeight"), QStringLiteral("44")},
        {QStringLiteral("int"), QStringLiteral("spacing4"), QStringLiteral("4")},
        {QStringLiteral("int"), QStringLiteral("spacing8"), QStringLiteral("8")},
        {QStringLiteral("int"), QStringLiteral("spacing12"), QStringLiteral("12")},
        {QStringLiteral("int"), QStringLiteral("spacing16"), QStringLiteral("16")},
        {QStringLiteral("int"), QStringLiteral("spacing20"), QStringLiteral("20")},
        {QStringLiteral("int"), QStringLiteral("spacing24"), QStringLiteral("24")},
        {QStringLiteral("int"), QStringLiteral("spacing28"), QStringLiteral("28")},
        {QStringLiteral("int"), QStringLiteral("spacing32"), QStringLiteral("32")},
        {QStringLiteral("int"), QStringLiteral("spacing40"), QStringLiteral("40")}
    };
    for (const QStringList &token : expectedTokens) {
        test.check(hasReadonlyPropertyValue(designTokens, token.at(0), token.at(1), token.at(2)),
                   QStringLiteral("DesignTokens must define exact template token %1 %2: %3")
                       .arg(token.at(0), token.at(1), token.at(2)));
    }

    const QStringList productPages{
        QStringLiteral("DashboardPage.qml"),
        QStringLiteral("SourcePage.qml"),
        QStringLiteral("CustomizePage.qml"),
        QStringLiteral("GpoStudioPage.qml"),
        QStringLiteral("UnattendedStudioPage.qml"),
        QStringLiteral("PackageStudioPage.qml"),
        QStringLiteral("WinForgeBridgePage.qml"),
        QStringLiteral("VmLabPage.qml"),
        QStringLiteral("PlanPage.qml"),
        QStringLiteral("HistoryPage.qml"),
        QStringLiteral("SettingsPage.qml"),
        QStringLiteral("TerminalPage.qml")
    };
    const QRegularExpression designTokenReferenceExpression(
        QStringLiteral(R"(\bDesignTokens\s*\.)"));
    for (const QString &page : productPages) {
        const QString path = sourceRoot + QStringLiteral("/qml/pages/") + page;
        const QString pageText = withoutComments(readText(path, &test));
        test.check(designTokenReferenceExpression.match(pageText).hasMatch(),
                   QStringLiteral("Product page qml/pages/%1 must use shared DesignTokens")
                       .arg(page));
    }

    const QString customizePage = readText(
        sourceRoot + QStringLiteral("/qml/pages/CustomizePage.qml"), &test);
    test.check(customizePage.contains(QStringLiteral("category: \"updates\""))
                   && customizePage.contains(QStringLiteral("items: app.updateCatalog"))
                   && customizePage.contains(QStringLiteral(
                       "addPayloadFiles(payloadPage.category")),
               QStringLiteral("Updates UI must use the typed ProjectConfig.updates catalog pipeline"));
    test.check(customizePage.contains(QStringLiteral("category: \"drivers\""))
                   && customizePage.contains(QStringLiteral("items: app.driverCatalog"))
                   && customizePage.contains(QStringLiteral("Import host drivers")),
               QStringLiteral("Drivers UI must expose inspected payloads and the host-driver source"));
    test.check(customizePage.contains(QStringLiteral("showAutomatic(payloadPage.category)"))
                   && customizePage.contains(QStringLiteral("sourceCatalogQuery"))
                   && customizePage.contains(QStringLiteral("FileDialog"))
                   && customizePage.contains(QStringLiteral("FolderDialog")),
               QStringLiteral("Driver/update acquisition must auto-match the ISO and retain local file/folder pickers"));
    test.check(customizePage.contains(QStringLiteral("component FeatureWorkbench"))
                   && customizePage.contains(QStringLiteral("setFeatureState("))
                   && customizePage.contains(QStringLiteral("featureDisables"))
                   && customizePage.contains(QStringLiteral("setCapabilityState("))
                   && customizePage.contains(QStringLiteral("capabilityChanges"))
                   && !customizePage.contains(QStringLiteral("component FeatureGrid")),
               QStringLiteral("Customize Features must expose tri-state feature and capability mutations"));
    test.check(customizePage.contains(QStringLiteral("component AppsWorkbench"))
                   && customizePage.contains(QStringLiteral("app.appRemovals"))
                   && customizePage.contains(QStringLiteral("app.appProvisions"))
                   && customizePage.contains(QStringLiteral("addAppxProvisionFiles(selectedFiles)"))
                   && customizePage.contains(QStringLiteral("*.appxbundle *.msix *.msixbundle")),
               QStringLiteral("Customize Apps must separate removal identities from signed provisioning files"));
    test.check(customizePage.contains(QStringLiteral("component ComponentsWorkbench"))
                   && customizePage.contains(QStringLiteral("app.componentRemovals"))
                   && customizePage.contains(QStringLiteral("app.scheduledTaskChanges"))
                   && customizePage.contains(QStringLiteral("setScheduledTaskChange("))
                   && customizePage.contains(QStringLiteral("taskCompatibilityOverride.checked")),
               QStringLiteral("Customize Components must expose guarded typed scheduled-task actions"));
    test.check(customizePage.contains(QStringLiteral("tryAddListItem"))
                   && customizePage.contains(QStringLiteral(
                       "if (configList.addAction(entry.text.trim()) !== false)"))
                   && customizePage.contains(QStringLiteral(
                       "featurePage.tr(\"Capability identity\", \"Capability 識別碼\")")),
               QStringLiteral("Customize list editors must retain rejected input and localize capability labels"));
    const QString mainQml = readText(
        sourceRoot + QStringLiteral("/qml/Main.qml"), &test);
    const QString mainPolicyText = withoutComments(mainQml);
    test.check(QRegularExpression(QStringLiteral(R"(\bDesignTokens\s*\.\s*navWidth\b)"))
                   .match(mainPolicyText).hasMatch()
                   && QRegularExpression(QStringLiteral(R"(\bDesignTokens\s*\.\s*navCompactWidth\b)"))
                          .match(mainPolicyText).hasMatch()
                   && QRegularExpression(QStringLiteral(R"(\bDesignTokens\s*\.\s*topBarHeight\b)"))
                          .match(mainPolicyText).hasMatch(),
               QStringLiteral("The application shell must consume the exact 260/76/64 navigation and top-bar tokens"));

    const QString navigationModel = arrayPropertyBlock(mainPolicyText,
                                                       QStringLiteral("navigationItems"));
    test.check(!navigationModel.isEmpty(),
               QStringLiteral("Main.qml navigationItems array could not be parsed"));
    const QRegularExpression allNavigationIconExpression(
        QStringLiteral(R"re(\bicon\s*:\s*"([^"]+)")re"));
    const QRegularExpression svgNavigationIconExpression(
        QStringLiteral(R"re(\bicon\s*:\s*"(qrc:/qt/qml/WimForge/assets/icons/[A-Za-z0-9_-]+\.svg)")re"));
    const QStringList allNavigationIcons = captureList(
        navigationModel, allNavigationIconExpression);
    const QStringList svgNavigationIcons = captureList(
        navigationModel, svgNavigationIconExpression);
    QSet<QString> actualNavigationIcons;
    for (const QString &path : svgNavigationIcons)
        actualNavigationIcons.insert(path);
    const QSet<QString> expectedNavigationIcons{
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/overview.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/source.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/customize.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/policy.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/unattended.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/package.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/bridge.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/vm.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/run.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/history.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/settings.svg"),
        QStringLiteral("qrc:/qt/qml/WimForge/assets/icons/terminal.svg")
    };
    QSet<QString> missingNavigationIcons = expectedNavigationIcons;
    missingNavigationIcons.subtract(actualNavigationIcons);
    QSet<QString> unexpectedNavigationIcons = actualNavigationIcons;
    unexpectedNavigationIcons.subtract(expectedNavigationIcons);
    test.check(svgNavigationIcons.size() == 12
                   && actualNavigationIcons.size() == 12
                   && missingNavigationIcons.isEmpty()
                   && unexpectedNavigationIcons.isEmpty(),
               QStringLiteral("Navigation must register the 12 template SVG paths (missing: %1; unexpected: %2)")
                   .arg(joinedSorted(missingNavigationIcons),
                        joinedSorted(unexpectedNavigationIcons)));
    test.check(allNavigationIcons == svgNavigationIcons
                   && !QRegularExpression(QStringLiteral(R"(\bglyph\s*:)") )
                           .match(navigationModel).hasMatch(),
               QStringLiteral("Navigation must use SVG QRC paths, not the legacy Unicode icon model"));

    const QString qrcPrefix = QStringLiteral("qrc:/qt/qml/WimForge/");
    for (const QString &qrcPath : expectedNavigationIcons) {
        const QString relativePath = qrcPath.mid(qrcPrefix.size());
        const QFileInfo svgFile(sourceRoot + QLatin1Char('/') + relativePath);
        test.check(svgFile.isFile() && svgFile.size() > 0,
                   QStringLiteral("Navigation SVG %1 must exist and be non-empty")
                       .arg(relativePath));
        test.check(cmakeLists.contains(relativePath),
                   QStringLiteral("Navigation SVG %1 must be registered as a QML resource")
                       .arg(relativePath));
    }

    test.check(mainQml.contains(QStringLiteral("en: \"Source & editions\""))
                   && mainQml.contains(QStringLiteral("en: \"Review & run\""))
                   && !mainQml.contains(QStringLiteral("buttonText(")),
               QStringLiteral("Visible navigation labels must render literal single ampersands"));
    test.check(mainQml.contains(QStringLiteral(
                   "if (ok) { openProjectSheet.close(); root.syncActiveWorkspaceTab() }")),
               QStringLiteral("Opening a project must restore its saved active workspace tab"));
    test.check(mainQml.contains(QStringLiteral("if (startupPageRequested)"))
                   && mainQml.contains(QStringLiteral("navigateToPage(startupPage)")),
               QStringLiteral("An explicit --page request must override restored tab state"));

    const QString projectStartPage = readText(
        sourceRoot + QStringLiteral("/qml/components/ProjectStartPage.qml"), &test);
    test.check(projectStartPage.contains(QStringLiteral("required property bool dark"))
                   && projectStartPage.contains(QStringLiteral("required property var tr"))
                   && projectStartPage.contains(QStringLiteral("property var recentProjects")),
               QStringLiteral("Project Start must receive theme, bilingual translation, and recent-project data"));
    for (const QString &signal : {
             QStringLiteral("createRequested"), QStringLiteral("openRequested"),
             QStringLiteral("importRequested"), QStringLiteral("recentRequested")}) {
        test.check(projectStartPage.contains(QStringLiteral("signal %1(").arg(signal)),
                   QStringLiteral("Project Start must expose the %1 integration signal")
                       .arg(signal));
    }
    test.check(projectStartPage.contains(QStringLiteral("removeRecentRequested"))
                   && projectStartPage.contains(QStringLiteral("clearRecentRequested"))
                   && projectStartPage.contains(QStringLiteral("DesignTokens.")),
               QStringLiteral("Project Start must support recent-list cleanup and shared Material tokens"));
    test.check(mainQml.contains(QStringLiteral("ProjectStartPage {"))
                   && mainQml.contains(QStringLiteral("visible: !app.projectLoaded"))
                   && mainQml.contains(QStringLiteral(
                       "recentProjects: projectStartCapture ? [] : app.recentProjects"))
                   && mainQml.contains(QStringLiteral("app.removeRecentProject(path)"))
                   && mainQml.contains(QStringLiteral("app.clearRecentProjects()")),
               QStringLiteral("Main must show Project Start before a project is loaded and wire recent-project actions"));

    const QString controllerSource = readText(
        sourceRoot + QStringLiteral("/src/AppController.cpp"), &test);
    test.check(controllerHeader.contains(QStringLiteral("Q_PROPERTY(QVariantList recentProjects"))
                   && controllerHeader.contains(QStringLiteral("removeRecentProject"))
                   && controllerHeader.contains(QStringLiteral("clearRecentProjects")),
               QStringLiteral("AppController must expose the recent-project model and cleanup actions"));
    test.check(controllerSource.contains(QStringLiteral("loadRecentProjects();"))
                   && controllerSource.contains(QStringLiteral("rememberRecentProject("))
                   && !controllerSource.contains(QStringLiteral("openProject(lastProject)"))
                   && !controllerSource.contains(QStringLiteral(
                       "setValue(QStringLiteral(\"project/last\")")),
               QStringLiteral("Normal startup must migrate recents without automatically reopening the last project"));
    test.check(controllerHeader.contains(QStringLiteral("Q_PROPERTY(QStringList featureDisables"))
                   && controllerHeader.contains(QStringLiteral("Q_PROPERTY(QVariantList capabilityChanges"))
                   && controllerHeader.contains(QStringLiteral("Q_PROPERTY(QStringList appProvisions"))
                   && controllerHeader.contains(QStringLiteral("Q_PROPERTY(QVariantList scheduledTaskChanges"))
                   && controllerHeader.contains(QStringLiteral("setFeatureState"))
                   && controllerHeader.contains(QStringLiteral("setCapabilityState"))
                   && controllerHeader.contains(QStringLiteral("addAppxProvisionFiles"))
                   && controllerHeader.contains(QStringLiteral("setScheduledTaskChange")),
               QStringLiteral("AppController must expose every typed Customize mutation to QML"));
    test.check(controllerSource.contains(QStringLiteral("imageInventory"))
                   && controllerSource.contains(QStringLiteral("QJsonArray::fromStringList(result.editions)"))
                   && controllerSource.contains(QStringLiteral(
                       "project.selectedImageIndex = qBound(1, project.selectedImageIndex,"))
                   && controllerSource.contains(QStringLiteral(
                       "m_project->selectedImageIndex = qBound(")),
               QStringLiteral("Source inspection must persist inventory metadata and clamp the selected index"));
    test.check(controllerHeader.contains(QStringLiteral("bool tryAddListItem"))
                   && controllerSource.contains(QStringLiteral(
                       "bool AppController::tryAddListItem")),
               QStringLiteral("Config-list mutations must report save/validation failure to QML"));
    test.check(controllerSource.contains(QStringLiteral(
                   "const QString commitMessage = bilingualCommitMessage("))
                   && controllerSource.contains(QStringLiteral(
                       "const QString reconnectMessage = bilingualCommitMessage("))
                   && controllerSource.contains(QStringLiteral(
                       "project.save(&error, commitMessage)"))
                   && controllerSource.contains(QStringLiteral(
                       "candidate.save(&error, reconnectMessage)"))
                   && controllerSource.contains(QStringLiteral(
                       "restored->save(&error, bilingualCommitMessage("))
                   && !controllerSource.contains(QStringLiteral(
                       "mutateProject(QStringLiteral(")),
               QStringLiteral("AppController-generated project commits must use bilingual subjects"));
    test.check(!controllerSource.contains(QStringLiteral(
                   "QJsonArray::fromStringList(command.arguments)"))
                   && !controllerSource.contains(QStringLiteral(
                       "{QStringLiteral(\"source\"), sourceAtLaunch}"))
                   && !controllerSource.contains(QStringLiteral(
                       "{QStringLiteral(\"output\"), result.output}"))
                   && !controllerSource.contains(QStringLiteral(
                       "{QStringLiteral(\"destination\"), destination}"))
                   && !controllerSource.contains(QStringLiteral(
                       "{QStringLiteral(\"project\"), projectRoot()}"))
                   && controllerSource.contains(QStringLiteral("outputBytes"))
                   && controllerSource.contains(QStringLiteral("argumentCount")),
               QStringLiteral("AppController JSONL metadata must retain diagnostics without paths, argv, or output text"));

    const QString settingsPage = withoutComments(readText(
        sourceRoot + QStringLiteral("/qml/pages/SettingsPage.qml"), &test));
    const QString settingsCategories = arrayPropertyBlock(
        settingsPage, QStringLiteral("categories"));
    const QStringList categoryNames = captureList(
        settingsCategories,
        QRegularExpression(QStringLiteral(R"re(\ben\s*:\s*"([^"]+)")re")));
    QSet<QString> actualSettingsCategories;
    for (const QString &category : categoryNames)
        actualSettingsCategories.insert(category);
    const QSet<QString> expectedSettingsCategories{
        QStringLiteral("Appearance & language"),
        QStringLiteral("Jobs & resources"),
        QStringLiteral("Safety"),
        QStringLiteral("Automation"),
        QStringLiteral("Diagnostics")
    };
    test.check(categoryNames.size() == 5
                   && actualSettingsCategories == expectedSettingsCategories,
               QStringLiteral("Settings must expose exactly the five template categories; found: %1")
                   .arg(joinedSorted(actualSettingsCategories)));
    const qsizetype settingsPanelCount = matchCount(
        settingsPage,
        QRegularExpression(QStringLiteral(R"(\bSettingsPanel\s*\{)")));
    test.check(settingsPanelCount == 5,
               QStringLiteral("Settings must define one category surface per template category; found %1 panels")
                   .arg(settingsPanelCount));
    test.check(!QRegularExpression(QStringLiteral(R"(\bGroupBox\b)"))
                   .match(settingsPage).hasMatch(),
               QStringLiteral("Settings must use the redesigned category surfaces, not legacy GroupBox cards"));
    test.check(!hasLeadingSymbolTitle(settingsPage),
               QStringLiteral("Settings titles must use text/semantic components, not emoji-prefixed title literals"));

    const QString sourcePage = readText(
        sourceRoot + QStringLiteral("/qml/pages/SourcePage.qml"), &test);
    test.check(sourcePage.contains(QStringLiteral(
                   "root.acceptSource(app.pathFromUrl(drop.urls[0]))"))
                   && !sourcePage.contains(QStringLiteral("drop.urls[0].toLocalFile()")),
               QStringLiteral("Source drag/drop must convert QML URLs through AppController"));
    test.check(sourcePage.contains(QStringLiteral("Choose and inspect ISO / image"))
                   && sourcePage.contains(QStringLiteral("label: root.tr(\"Selected source\""))
                   && sourcePage.contains(QStringLiteral("label: root.tr(\"Image path\""))
                   && sourcePage.contains(QStringLiteral("label: root.tr(\"Mount directory\""))
                   && sourcePage.contains(QStringLiteral("Accessible.name: root.tr(\"Edition\""))
                   && sourcePage.contains(QStringLiteral("Accessible.name: root.tr(\"Image index\""))
                   && sourcePage.contains(QStringLiteral("sourceCatalogQuery")),
               QStringLiteral("Source must be one guided auto-inspection flow with stable accessible names"));

    const QString fieldComponent = readText(
        sourceRoot + QStringLiteral("/qml/components/WfField.qml"), &test);
    const QString headerComponent = readText(
        sourceRoot + QStringLiteral("/qml/components/WfPageHeader.qml"), &test);
    const QString catalogSheet = readText(
        sourceRoot + QStringLiteral("/qml/components/UpdateCatalogSheet.qml"), &test);
    test.check(fieldComponent.contains(QStringLiteral("Accessible.description: root.accessibleDescription"))
                   && !headerComponent.contains(QStringLiteral("maximumLineCount: 3")),
               QStringLiteral("Shared fields must expose descriptions and page headers must not clip bilingual copy"));
    test.check(catalogSheet.contains(QStringLiteral("modal: false"))
                   && catalogSheet.contains(QStringLiteral("showAutomatic(targetCategory)"))
                   && catalogSheet.contains(QStringLiteral("Download %1")),
               QStringLiteral("Update Catalog results must be non-blocking, ISO-driven, and uniquely named for accessibility"));
    test.check(mainQml.contains(QStringLiteral("compactToolbar"))
                   && mainQml.contains(QStringLiteral("Accessible.role: Accessible.PageTab"))
                   && mainQml.contains(QStringLiteral("Accessible.selected:"))
                   && mainQml.contains(QStringLiteral("activeFocusOnTab: true")),
               QStringLiteral("The shell must avoid narrow-toolbar clipping and expose keyboard-accessible workspace tabs"));

    const QString terminalPage = readText(
        sourceRoot + QStringLiteral("/qml/pages/TerminalPage.qml"), &test);
    const QString winForgePage = readText(
        sourceRoot + QStringLiteral("/qml/pages/WinForgeBridgePage.qml"), &test);
    test.check(settingsPage.contains(QStringLiteral("autoExportDialog"))
                   && terminalPage.contains(QStringLiteral("workingDirectoryDialog"))
                   && winForgePage.contains(QStringLiteral("runtimeFolderDialog"))
                   && winForgePage.contains(QStringLiteral("isoStagingFolderDialog")),
               QStringLiteral("Important file and directory path fields must provide consistent browse controls"));
    if (test.failures() == 0)
        QTextStream(stdout) << "QML/AppController contract is valid across " << qmlFileCount
                            << " files.\n";
    return test.failures() == 0 ? 0 : 1;
}
