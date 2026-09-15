#include "TestHarness.hpp"
#include "logic/navigation/PaneController.hpp"

#include <QCoreApplication>

namespace {

OPENSCP_TEST(testLiteralPattern, test) {
    QString error;
    const auto expression = openscpui::PaneController::compileSearchPattern(
        QStringLiteral("report"), &error);
    test.check(expression.isValid() && error.isEmpty(),
               "literal search patterns should compile");
    test.check(expression.match(QStringLiteral("Annual REPORT.pdf")).hasMatch(),
               "literal searches should be substring and case insensitive");
}

OPENSCP_TEST(testWildcardPattern, test) {
    const auto expression = openscpui::PaneController::compileSearchPattern(
        QStringLiteral("report-??.pdf"));
    test.check(expression.match(QStringLiteral("report-01.pdf")).hasMatch(),
               "question-mark wildcards should match one character");
    test.check(
        !expression.match(QStringLiteral("old-report-01.pdf")).hasMatch(),
        "wildcard searches should remain anchored");
}

OPENSCP_TEST(testRegularExpression, test) {
    const auto expression = openscpui::PaneController::compileSearchPattern(
        QStringLiteral("^invoice_[0-9]+\\.csv$"));
    test.check(expression.match(QStringLiteral("invoice_42.csv")).hasMatch(),
               "explicit regular expressions should remain supported");
    test.check(!expression.match(QStringLiteral("invoice_x.csv")).hasMatch(),
               "regular-expression constraints should be preserved");
}

OPENSCP_TEST(testInvalidExpression, test) {
    QString error;
    const auto expression = openscpui::PaneController::compileSearchPattern(
        QStringLiteral("[unfinished"), &error);
    test.check(!expression.isValid() && !error.isEmpty(),
               "invalid regular expressions should return their error");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("pane controller");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
