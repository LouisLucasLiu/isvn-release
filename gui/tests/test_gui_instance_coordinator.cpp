#include "backend/GuiInstanceCoordinator.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using isvn::gui::GuiInstanceCoordinator;

class GuiInstanceCoordinatorTest final : public QObject {
    Q_OBJECT

private slots:
    void sameProjectActivatesAndDifferentModeConflicts() {
        QTemporaryDir runtime;
        QVERIFY(runtime.isValid());
        qputenv("ISVN_GUI_TEST_RUNTIME_DIR", runtime.path().toUtf8());

        GuiInstanceCoordinator owner;
        QString error;
        QCOMPARE(owner.acquire(QStringLiteral("project-a"), QStringLiteral("new"), &error),
                 GuiInstanceCoordinator::AcquireResult::Owner);
        QSignalSpy activated(&owner, &GuiInstanceCoordinator::activationRequested);

        GuiInstanceCoordinator second;
        QCOMPARE(second.acquire(QStringLiteral("project-a"), QStringLiteral("new"), &error),
                 GuiInstanceCoordinator::AcquireResult::ActivatedExisting);
        QTRY_COMPARE(activated.count(), 1);

        GuiInstanceCoordinator conflicting;
        QCOMPARE(conflicting.acquire(QStringLiteral("project-a"), QStringLiteral("legacy"), &error),
                 GuiInstanceCoordinator::AcquireResult::ModeConflict);
        QVERIFY(error.contains(QStringLiteral("different shell mode")));

        GuiInstanceCoordinator independent;
        QCOMPARE(independent.acquire(QStringLiteral("project-b"), QStringLiteral("new"), &error),
                 GuiInstanceCoordinator::AcquireResult::Owner);
        qunsetenv("ISVN_GUI_TEST_RUNTIME_DIR");
    }
};

QTEST_MAIN(GuiInstanceCoordinatorTest)
#include "test_gui_instance_coordinator.moc"
