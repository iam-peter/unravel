#include "flow.h"

#include <QCoreApplication>
#include <QTextStream>

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const QStringList args = app.arguments();
  QString project, file, name;
  int line = 0;
  for (int i = 1; i < args.size(); ++i) {
    const QString key = args[i];
    if (i + 1 >= args.size()) {
      QTextStream(stderr) << "Missing value for " << key << '\n';
      return 2;
    }
    const QString value = args[++i];
    if (key == QLatin1String("--project"))
      project = value;
    else if (key == QLatin1String("--file"))
      file = value;
    else if (key == QLatin1String("--line"))
      line = value.toInt();
    else if (key == QLatin1String("--qualified-name"))
      name = value;
    else {
      QTextStream(stderr) << "Unknown option: " << key << '\n';
      return 2;
    }
  }
  if (project.isEmpty() || file.isEmpty() || line < 1 || name.isEmpty()) {
    QTextStream(stderr)
        << "Usage: unravel-flow --project <root-or-compile_commands.json> "
           "--file <source> --line <definition-line> --qualified-name <name>\n";
    return 2;
  }
  QString error;
  const QByteArray output =
      Unravel::Flow::generate(project, file, line, name, &error);
  if (output.isEmpty()) {
    QTextStream(stderr) << error << '\n';
    return 1;
  }
  QTextStream(stdout) << QString::fromUtf8(output) << '\n';
  return 0;
}
