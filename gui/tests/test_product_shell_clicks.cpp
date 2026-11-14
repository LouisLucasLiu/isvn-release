#include <QColor>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>

#include <memory>

class ProductShellClicksTest final : public QObject {
    Q_OBJECT

private slots:
    void persistentShellOwnsFourPagesAndNoDuplicateChrome() {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                              QStringLiteral("/qml/shell/ProductShell.qml")));
        std::unique_ptr<QObject> object(component.create());
        if (!object) qFatal("%s", qPrintable(component.errorString()));
        auto* shell = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(shell != nullptr);
        QQuickWindow window;
        window.resize(1600, 900);
        shell->setParentItem(window.contentItem());
        shell->setSize(window.size());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        QVERIFY(shell->findChild<QObject*>(QStringLiteral("shellTopBar")) != nullptr);
        QVERIFY(shell->findChild<QObject*>(QStringLiteral("shellBottomBar")) != nullptr);
        QVERIFY(shell->findChild<QObject*>(QStringLiteral("homePageLoader")) != nullptr);
        QVERIFY(shell->findChild<QObject*>(QStringLiteral("workPage")) != nullptr);
        QVERIFY(shell->findChild<QObject*>(QStringLiteral("explorePageLoader")) != nullptr);
        QVERIFY(shell->findChild<QObject*>(QStringLiteral("maintenancePageLoader")) != nullptr);

        shell->setProperty("currentPage", QStringLiteral("Explore"));
        QCoreApplication::processEvents();
        auto* explorer = shell->findChild<QObject*>(QStringLiteral("explorePageLoader"));
        QVERIFY(explorer != nullptr);
        QCOMPARE(explorer->property("visible").toBool(), true);
        // The ProductShell itself owns one navigation frame; embedded B/C are
        // body components and never create a second ProductNavigation.
        QCOMPARE(shell->findChildren<QObject*>(QStringLiteral("productNavigation")).size(), 1);
    }

    // The Work panels and the Explore version picker draw their own surface.
    // That surface must come from the active theme, otherwise the theme's text
    // colour lands on a fixed white background and disappears in dark mode.
    void workPanelsAndVersionPickerFollowTheTheme() {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                              QStringLiteral("/qml/shell/ProductShell.qml")));
        std::unique_ptr<QObject> object(component.create());
        if (!object) qFatal("%s", qPrintable(component.errorString()));
        auto* shell = qobject_cast<QQuickItem*>(object.get());
        QVERIFY(shell != nullptr);
        QQuickWindow window;
        window.resize(1600, 900);
        shell->setParentItem(window.contentItem());
        shell->setSize(window.size());
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        auto* theme = shell->findChild<QObject*>(QStringLiteral("workTheme"));
        QVERIFY(theme != nullptr);
        auto* navigator = shell->findChild<QObject*>(QStringLiteral("localVersionNavigator"));
        auto* files = shell->findChild<QObject*>(QStringLiteral("fileStatusWorkspace"));
        auto* centerRight = shell->findChild<QObject*>(QStringLiteral("workCenterRight"));
        QVERIFY(navigator != nullptr);
        QVERIFY(files != nullptr);
        QVERIFY(centerRight != nullptr);
        QObject* workflow = nullptr;
        for (QObject* candidate : centerRight->findChildren<QObject*>()) {
            if (candidate->metaObject()->indexOfProperty("realBinding") >= 0 &&
                candidate->metaObject()->indexOfProperty("actionController") >= 0) {
                workflow = candidate;
                break;
            }
        }
        QVERIFY(workflow != nullptr);

        shell->setProperty("currentPage", QStringLiteral("Explore"));
        QCoreApplication::processEvents();
        auto* picker = shell->findChild<QObject*>(QStringLiteral("versionPickerDialog"));
        QVERIFY(picker != nullptr);
        auto* pickerBackground = picker->property("background").value<QQuickItem*>();
        QVERIFY(pickerBackground != nullptr);

        for (bool dark : {true, false}) {
            theme->setProperty("dark", dark);
            QCoreApplication::processEvents();
            const QColor panel = theme->property("bgPanel").value<QColor>();
            QCOMPARE(navigator->property("color").value<QColor>(), panel);
            QCOMPARE(files->property("color").value<QColor>(), panel);
            QCOMPARE(workflow->property("color").value<QColor>(), panel);
            QCOMPARE(pickerBackground->property("color").value<QColor>(), panel);
        }
    }

    void exploreExplicitActivationUsesRevalidationRoute() {
        QQmlEngine engine;
        engine.addImportPath(QStringLiteral(ISVN_GUI_SOURCE_DIR) + QStringLiteral("/qml"));
        QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(ISVN_GUI_SOURCE_DIR) +
                                                              QStringLiteral("/qml/work/WorkReadController.qml")));
        std::unique_ptr<QObject> controller(component.create());
        if (!controller) qFatal("%s", qPrintable(component.errorString()));
        QVERIFY(QMetaObject::invokeMethod(controller.get(), "revalidateAndSelect",
                                          Q_ARG(QVariant, QVariant(QStringLiteral("P/B/S/V")))));
        QCOMPARE(controller->property("_pendingExplicitActive").toString(), QStringLiteral("P/B/S/V"));
    }
};

QTEST_MAIN(ProductShellClicksTest)
#include "test_product_shell_clicks.moc"
