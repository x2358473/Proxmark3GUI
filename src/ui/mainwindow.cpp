#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <QDirIterator>
#include <QJsonDocument>
#include <QTimer>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QDir>
#include <QRegularExpressionValidator> // 如果没有包含，请在顶部加上这个以支持正则输入限制

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow) {
  ui->setupUi(this);
  dockAllWindows = new QAction(tr("Dock all windows"), this);
  myInfo = new QAction("wh201906", this);
  currVersion = new QAction(
      tr("Ver: ") + QApplication::applicationVersion().section('.', 0, -2),
      this); // ignore the 4th version number
  checkUpdate = new QAction(tr("Check Update"), this);
  connect(dockAllWindows, &QAction::triggered, [=]() {
    for (int i = 0; i < dockList.size(); i++)
      dockList[i]->setFloating(false);
  });
  connect(myInfo, &QAction::triggered, [=]() {
    QDesktopServices::openUrl(QUrl("https://github.com/wh201906"));
  });
  connect(checkUpdate, &QAction::triggered, [=]() {
    QDesktopServices::openUrl(
        QUrl("https://github.com/wh201906/Proxmark3GUI/releases"));
  });

  settings = new QSettings("GUIsettings.ini", QSettings::IniFormat);
  settings->setIniCodec("UTF-8");

  pm3Thread = new QThread(this);
  connect(QApplication::instance(), &QApplication::aboutToQuit, pm3Thread,
          &QThread::quit);
  pm3 = new PM3Process(pm3Thread);
  connect(pm3Thread, &QThread::finished, pm3, &PM3Process::deleteLater);
  pm3Thread->start();
  pm3state = false;
  clientWorkingDir = new QDir;

  util = new Util(this);
  Util::setUI(ui);
  mifare = new Mifare(ui, util, this);
  lf = new LF(ui, util, this);
  t55xxTab = new T55xxTab(util);
  connect(lf, &LF::LFfreqConfChanged, this, &MainWindow::onLFfreqConfChanged);
  connect(t55xxTab, &T55xxTab::setParentGUIState, this, &MainWindow::setState);
  ui->funcTab->insertTab(2, t55xxTab, tr("T55xx"));

  keyEventFilter = new MyEventFilter(QEvent::KeyPress);
  resizeEventFilter = new MyEventFilter(QEvent::Resize);

  // hide unused tabs
  //    ui->funcTab->removeTab(1);
  ui->funcTab->removeTab(3);

  portSearchTimer = new QTimer(this);
  portSearchTimer->setInterval(2000);
  connect(portSearchTimer, &QTimer::timeout, this,
          &MainWindow::on_portSearchTimer_timeout);
  portSearchTimer->start();

  contextMenu = new QMenu();
  contextMenu->addAction(dockAllWindows);
  contextMenu->addSeparator();
  contextMenu->addAction(myInfo);
  currVersion->setEnabled(false);
  contextMenu->addAction(currVersion);
  contextMenu->addAction(checkUpdate);
}

MainWindow::~MainWindow() {
  delete ui;
  emit killPM3();
  pm3Thread->exit(0);
  pm3Thread->wait(5000);
  delete pm3;
  delete pm3Thread;
}

void MainWindow::loadConfig() {
  QString filename = ui->Set_Client_configFileBox->currentData().toString();
  if (filename == "(ext)")
    filename = ui->Set_Client_configPathEdit->text();
  qDebug() << "config file:" << filename;
  QFile configList(filename);
  if (!configList.open(QFile::ReadOnly | QFile::Text)) {
    QMessageBox::information(this, tr("Info"),
                             tr("Failed to load config file"));
    return;
  }

  QByteArray configData = configList.readAll();
  QJsonDocument configJson(QJsonDocument::fromJson(configData));
  mifare->setConfigMap(
      configJson.object()["mifare classic"].toObject().toVariantMap());
  lf->setConfigMap(configJson.object()["lf"].toObject().toVariantMap());
  t55xxTab->setConfigMap(
      configJson.object()["t55xx"].toObject().toVariantMap());
}

void MainWindow::initUI() // will be called by main.app
{
  ui->retranslateUi(this);
  uiInit();
  signalInit();
  setState(false);
  dockInit();
}

// ******************** basic functions ********************

void MainWindow::on_portSearchTimer_timeout() {
  QStringList newPortList;     // for actural port name
  QStringList newPortNameList; // for display name
  const QString hint = " *";

  foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
    //        qDebug() << info.isNull() << info.portName() << info.description()
    //        << info.serialNumber() << info.manufacturer();
    if (!info.isNull()) {
      QString idString =
          (info.description() + info.serialNumber() + info.manufacturer())
              .toLower();
      QString portName = info.portName();

      newPortList << portName;
      if (info.hasVendorIdentifier() && info.hasProductIdentifier()) {
        quint16 vid = info.vendorIdentifier();
        quint16 pid = info.productIdentifier();
        if (vid == 0x9AC4 && pid == 0x4B8F)
          portName += hint;
        else if (vid == 0x2D2D && pid == 0x504D)
          portName += hint;
      } else if (idString.contains("proxmark") || idString.contains("iceman"))
        portName += hint;
      newPortNameList << portName;
    }
  }
  if (newPortList !=
      portList) // update PM3_portBox when available ports changed
  {
    portList = newPortList;
    ui->PM3_portBox->clear();
    int selectId = -1;
    for (int i = 0; i < portList.size(); i++) {
      ui->PM3_portBox->addItem(newPortNameList[i], newPortList[i]);
      if (selectId == -1 && newPortNameList[i].endsWith(hint))
        selectId = i;
    }
    if (selectId != -1)
      ui->PM3_portBox->setCurrentIndex(selectId);
  }
}

void MainWindow::on_PM3_connectButton_clicked() {
  qDebug() << "Main:" << QThread::currentThread();

  const QComboBox *portBox = ui->PM3_portBox;
  QString port;
  if (portBox->currentText() == portBox->itemText(portBox->currentIndex()))
    // in the list
    port = portBox->currentData().toString();
  else
    // not in the list
    port = portBox->currentText();
  qDebug() << "port:" << port;
  QString startArgs = ui->Set_Client_startArgsEdit->text();
  QString clientPath = ui->PM3_pathBox->currentText();
  QFileInfo clientFile(clientPath);
  bool clientExist = false;

  QStringList extList = {""};
#ifdef Q_OS_WIN
  if (clientFile.suffix().isEmpty()) {
    QString pathExt = QProcessEnvironment::systemEnvironment().value("pathext");
    extList += pathExt.split(";", Qt::SkipEmptyParts);
    if (extList.size() == 1)
      extList += ".exe";
  }
#endif
  for (const QString &ext : extList) {
    QFileInfo executable(clientFile.filePath() + ext);
    if (executable.isFile()) {
      clientExist = true;
      break;
    }
  }

  if (!clientExist) {
    QMessageBox::information(this, tr("Info"), tr("The client path is invalid"),
                             QMessageBox::Ok);
    return;
  }

  // on RRG repo, if no port is specified, the client will search the available
  // port
  if (port == "" && startArgs.contains("<port>")) // has <port>, no port
  {
    QMessageBox::information(this, tr("Info"), tr("Plz choose a port first"),
                             QMessageBox::Ok);
    return;
  }

  if (!startArgs.contains("<port>")) // no <port>
    port = "";                       // a symbol

  QStringList args = startArgs.replace("<port>", port).split(' ');
  addClientPath(clientPath);

  QProcess envSetProcess;
  QString envScriptPath = ui->Set_Client_envScriptEdit->text();
  if (envScriptPath.contains("<client dir>"))
    envScriptPath.replace("<client dir>",
                          clientFile.absoluteDir().absolutePath());

  QFileInfo envScript(envScriptPath);
  if (envScript.exists()) {
    qDebug() << envScript.absoluteFilePath();
    // use the shell session to keep the environment then read it
#ifdef Q_OS_WIN
    // cmd /c "<path>">>nul && set
    envSetProcess.start(
        "cmd", {}, QProcess::Unbuffered | QProcess::ReadWrite | QProcess::Text);
    envSetProcess.write(
        QString("\"" + envScript.absoluteFilePath() + "\">>nul\n").toLatin1());
    envSetProcess.waitForReadyRead(10000);
    envSetProcess.readAll();
    envSetProcess.write("set\n");
#else
    // need implementation(or test if space works)
    // sh -c '. "<path>">>/dev/null && env'
    envSetProcess.start("sh -c \' . \"" + envScript.absoluteFilePath() +
                        "\">>/dev/null && env");
#endif
    envSetProcess.waitForReadyRead(10000);
    QString envSetResult = QString(envSetProcess.readAll());
#if (QT_VERSION <= QT_VERSION_CHECK(5, 14, 0))
    clientEnv = envSetResult.split("\n", QString::SkipEmptyParts);
#else
    clientEnv = envSetResult.split("\n", Qt::SkipEmptyParts);
#endif
    if (clientEnv.size() > 2) // the first element is "set" and the last element
                              // is the current path
    {
      clientEnv.removeFirst();
      clientEnv.removeLast();
      emit setProcEnv(&clientEnv);
    }
    //      qDebug() << "Get Env List" << clientEnv;
  } else
    clientEnv.clear();

  clientWorkingDir->setPath(QApplication::applicationDirPath());
  qDebug() << clientWorkingDir->absolutePath();
  clientWorkingDir->mkpath(ui->Set_Client_workingDirEdit->text());
  qDebug() << clientWorkingDir->absolutePath();
  clientWorkingDir->cd(ui->Set_Client_workingDirEdit->text());
  qDebug() << clientWorkingDir->absolutePath();
  emit setWorkingDir(clientWorkingDir->absolutePath());

  loadConfig();
  emit connectPM3(clientPath, args);
  if (port != "" && !keepClientActive)
    emit setSerialListener(port, true);
  else if (!keepClientActive)
    emit setSerialListener(false);

  envSetProcess.kill();
}

void MainWindow::onPM3ErrorOccurred(QProcess::ProcessError error) {
  qDebug() << "PM3 Error:" << error << pm3->errorString();
  if (error == QProcess::FailedToStart)
    QMessageBox::information(this, tr("Info"),
                             tr("Failed to start the client") + "\n" +
                                 pm3->errorString());
}

void MainWindow::onPM3HWConnectFailed() {
  QMessageBox::information(this, tr("Info"),
                           tr("Failed to connect to the hardware"));
}

void MainWindow::onPM3StateChanged(bool st, const QString &info) {
  pm3state = st;
  setState(st);
  if (st == true) {
    portSearchTimer->stop();
    setStatusBar(PM3VersionBar, info);
    setStatusBar(connectStatusBar, tr("Connected"));
  } else {
    portSearchTimer->start();
    setStatusBar(PM3VersionBar, "");
    setStatusBar(connectStatusBar, tr("Not Connected"));
  }
}

void MainWindow::on_PM3_disconnectButton_clicked() {
  emit killPM3();
  emit setSerialListener(false);
}

void MainWindow::refreshOutput(const QString &output) {
  // 原有的控制台文本插入逻辑
  ui->Raw_outputEdit->moveCursor(QTextCursor::End);
  ui->Raw_outputEdit->insertPlainText(output);
  ui->Raw_outputEdit->moveCursor(QTextCursor::End);

  // ==========================================
  // 智能破解建议逻辑区 (防连弹延迟触发版)
  // ==========================================

  // 使用静态变量保持状态跨函数调用的存活
  static QTimer* suggestTimer = nullptr;
  static int currentVulnLevel = 0; // 威胁等级: 0:无, 1:强化卡, 2:弱随机数, 3:静态随机数, 4:三代卡

  // 初始化一个只触发一次的定时器
  if (!suggestTimer) {
      suggestTimer = new QTimer(this);
      suggestTimer->setSingleShot(true);
      suggestTimer->setInterval(500);

      connect(suggestTimer, &QTimer::timeout, this, [this]() {
          QString allText = ui->Raw_outputEdit->toPlainText();
          QString prefix = "";
          if (allText.contains("Gen 2 / CUID", Qt::CaseInsensitive)) {
              prefix = tr("检测到 Gen 2 / CUID 魔术卡：\n\n");
          }

          if (currentVulnLevel == 4) {
              QMessageBox::information(this, tr("三代卡检测"),
                                       prefix + tr("【读取到复旦三代无漏洞卡 (FM11RF08S)】\n\n该卡免疫传统的 Nested 攻击。\n\n👉 建议步骤：直接点击【解三代卡】运行脚本。"));
          }
          else if (currentVulnLevel == 3) {
              QMessageBox::information(this, tr("破解建议"),
                                       prefix + tr("【检测到静态随机数 (Static Nonce) 漏洞】\n\n👉 操作建议：\n1. 请先点击上方【(2)扫描默认密码】获取至少一个密钥。\n2. 再点击【知一求全】破解，程序将极速秒解。"));
          }
          else if (currentVulnLevel == 2) {
              QMessageBox::information(this, tr("破解建议"),
                                       prefix + tr("【检测到弱随机数 (Weak PRNG) 漏洞】\n\n👉 操作建议：\n1. 请先点击上方【(2)扫描默认密码】。\n2. 再点击【知一求全】进行常规破解。"));
          }
          else if (currentVulnLevel == 1) {
              QMessageBox::information(this, tr("破解建议"),
                                       prefix + tr("【检测到强化加密 (Hardened) 卡片】\n\n该卡已修复常规漏洞。\n\n👉 操作建议：\n1. 请先点击上方【(2)扫描默认密码】碰碰运气。\n2. 如果扫描到了至少一个密码，使用【Hardnested攻击】进行深度破解。"));
          }

          currentVulnLevel = 0;
      });
  }

  // 实时解析每一行，但只提升威胁等级，绝不立刻弹窗
  bool needStartTimer = false;

  if (output.contains("Hint: Try `script run fm11rf08s_recovery.py", Qt::CaseInsensitive)) {
      if (currentVulnLevel < 4) currentVulnLevel = 4;
      needStartTimer = true;
  }
  else if (output.contains("[+] Static nonce... yes", Qt::CaseInsensitive) ||
           output.contains("[+] Static enc nonce... yes", Qt::CaseInsensitive)) {
      if (currentVulnLevel < 3) currentVulnLevel = 3;
      needStartTimer = true;
  }
  else if (output.contains("[+] Prng....... weak", Qt::CaseInsensitive)) {
      if (currentVulnLevel < 2) currentVulnLevel = 2;
      needStartTimer = true;
  }
  else if (output.contains("Hardened MIFARE Classic", Qt::CaseInsensitive)) {
      if (currentVulnLevel < 1) currentVulnLevel = 1;
      needStartTimer = true;
  }

  if (needStartTimer) {
      suggestTimer->start(); // 如果计时器正在跑，再次调用 start() 会重置倒计时 (防抖的核心)
  }


  // 1. 抓取并加载密钥文件 (key.bin)
  // ==========================================
  // 自动抓取与加载文件逻辑区 (兼容 rf08s 与 autopwn)
  // ==========================================

  // 1. 抓取并加载密钥文件
  QRegularExpression keyRegex("(?:saved to file|dumped to)\\s+[`']?([^`'\\n\\r]+-key(?:-[0-9]+)?\\.bin)[`']?", QRegularExpression::CaseInsensitiveOption);
  QRegularExpressionMatch keyMatch = keyRegex.match(output);
  if (keyMatch.hasMatch()) {
      QString filePath = keyMatch.captured(1).trimmed();
      QFileInfo fileInfo(filePath);
      QString fullPath = filePath;

      // 如果 PM3 给的是相对路径，智能判断它到底保存在哪
      if (!fileInfo.isAbsolute()) {
          QString pathInWorkDir = clientWorkingDir->absolutePath() + "/" + filePath;
          QString pathInHomeDir = QDir::homePath() + "/" + filePath;

          if (QFile::exists(pathInWorkDir)) {
              fullPath = pathInWorkDir;
          } else if (QFile::exists(pathInHomeDir)) {
              fullPath = pathInHomeDir;
          } else {
              fullPath = pathInWorkDir; // 兜底方案
          }
      }

      util->delay(500);
      mifare->data_loadKeyFile(fullPath);
  }

  // 2. 抓取并加载数据文件
  QRegularExpression dumpRegex("(?:saved to file|dumped to|to binary file)\\s+[`']?([^`'\\n\\r]+-dump(?:-[0-9]+)?\\.bin)[`']?", QRegularExpression::CaseInsensitiveOption);
  QRegularExpressionMatch dumpMatch = dumpRegex.match(output);
  if (dumpMatch.hasMatch()) {
      QString filePath = dumpMatch.captured(1).trimmed();
      QFileInfo fileInfo(filePath);
      QString fullPath = filePath;

      // 同样的智能路径判断逻辑
      if (!fileInfo.isAbsolute()) {
          QString pathInWorkDir = clientWorkingDir->absolutePath() + "/" + filePath;
          QString pathInHomeDir = QDir::homePath() + "/" + filePath;

          if (QFile::exists(pathInWorkDir)) {
              fullPath = pathInWorkDir;
          } else if (QFile::exists(pathInHomeDir)) {
              fullPath = pathInHomeDir;
          } else {
              fullPath = pathInWorkDir;
          }
      }

      util->delay(500);

      if (mifare->data_loadDataFile(fullPath)) {
          mifare->data_data2Key(); // 提取密码

          ui->funcTab->setCurrentIndex(0);
          // 将原来的“破解成功”替换为更严谨的“加载成功”
          QMessageBox::information(this, tr("数据加载成功"),
                                   tr("操作完成！\n已成功读取 Dump 数据文件，卡片数据及密码已自动同步至数据面板。"));
      }
  }
}

void MainWindow::on_stopButton_clicked() {
  if (!pm3state)
    on_PM3_disconnectButton_clicked();
  else {
    on_PM3_disconnectButton_clicked();
    for (int i = 0; i < 10; i++) {
      util->delay(200);
      if (!pm3state)
        break;
    }
    emit reconnectPM3();
    emit setSerialListener(!keepClientActive);
  }
}
// *********************************************************

// ******************** raw command ********************

void MainWindow::on_Raw_CMDEdit_textChanged(const QString &arg1) {
  stashedCMDEditText = arg1;
}

void MainWindow::on_Raw_sendCMDButton_clicked() {
  util->execCMD(ui->Raw_CMDEdit->text());
  refreshCMD(ui->Raw_CMDEdit->text());
}

void MainWindow::on_Raw_clearOutputButton_clicked() {
  ui->Raw_outputEdit->clear();
}

void MainWindow::on_Raw_CMDHistoryBox_stateChanged(int arg1) {
  if (arg1 == Qt::Checked) {
    ui->Raw_CMDHistoryWidget->setVisible(true);
    ui->Raw_clearHistoryButton->setVisible(true);
    ui->Raw_CMDHistoryBox->setText(tr("History:"));
  } else {
    ui->Raw_CMDHistoryWidget->setVisible(false);
    ui->Raw_clearHistoryButton->setVisible(false);
    ui->Raw_CMDHistoryBox->setText("");
  }
}

void MainWindow::on_Raw_clearHistoryButton_clicked() {
  ui->Raw_CMDHistoryWidget->clear();
}

void MainWindow::on_Raw_CMDHistoryWidget_itemDoubleClicked(
    QListWidgetItem *item) {
  ui->Raw_CMDEdit->setText(item->text());
  ui->Raw_CMDEdit->setFocus();
}

void MainWindow::sendMSG() // send command when pressing Enter
{
  if (ui->Raw_CMDEdit->hasFocus())
    on_Raw_sendCMDButton_clicked();
}

void MainWindow::refreshCMD(const QString &cmd) {
  ui->Raw_CMDEdit->blockSignals(true);
  ui->Raw_CMDEdit->setText(cmd);
  if (cmd != "" &&
      (ui->Raw_CMDHistoryWidget->count() == 0 ||
       ui->Raw_CMDHistoryWidget->item(ui->Raw_CMDHistoryWidget->count() - 1)
               ->text() != cmd))
    ui->Raw_CMDHistoryWidget->addItem(cmd);
  stashedCMDEditText = cmd;
  stashedIndex = -1;
  ui->Raw_CMDEdit->blockSignals(false);
}

void MainWindow::on_Raw_keyPressed(QObject *obj_addr, QEvent &event) {
  if (event.type() == QEvent::KeyPress) {
    QKeyEvent &keyEvent = static_cast<QKeyEvent &>(event);
    if (obj_addr == ui->Raw_CMDEdit) {
      if (keyEvent.key() == Qt::Key_Up) {
        if (stashedIndex > 0)
          stashedIndex--;
        else if (stashedIndex == -1)
          stashedIndex = ui->Raw_CMDHistoryWidget->count() - 1;
      } else if (keyEvent.key() == Qt::Key_Down) {
        if (stashedIndex < ui->Raw_CMDHistoryWidget->count() - 1 &&
            stashedIndex != -1)
          stashedIndex++;
        else if (stashedIndex == ui->Raw_CMDHistoryWidget->count() - 1)
          stashedIndex = -1;
      }
      if (keyEvent.key() == Qt::Key_Up || keyEvent.key() == Qt::Key_Down) {
        ui->Raw_CMDEdit->blockSignals(true);
        if (stashedIndex == -1)
          ui->Raw_CMDEdit->setText(stashedCMDEditText);
        else
          ui->Raw_CMDEdit->setText(
              ui->Raw_CMDHistoryWidget->item(stashedIndex)->text());
        ui->Raw_CMDEdit->blockSignals(false);
      }
    } else if (obj_addr == ui->Raw_outputEdit) {
      if (keyEvent.key() == Qt::Key_Up || keyEvent.key() == Qt::Key_Down)
        ui->Raw_CMDEdit->setFocus();
    }
  }
}
// *****************************************************

// ******************** mifare ********************
void MainWindow::on_MF_keyWidget_resized(QObject *obj_addr, QEvent &event) {
  if (obj_addr == ui->MF_keyWidget && event.type() == QEvent::Resize) {
    QTableWidget *widget = (QTableWidget *)obj_addr;
    int keyItemWidth = widget->width();
    keyItemWidth -= widget->verticalScrollBar()->width();
    keyItemWidth -= 2 * widget->frameWidth();
    keyItemWidth -= widget->horizontalHeader()->sectionSize(0);
    widget->horizontalHeader()->resizeSection(1, keyItemWidth / 2);
    widget->horizontalHeader()->resizeSection(2, keyItemWidth / 2);
  }
}

void MainWindow::MF_onMFCardTypeChanged(int id, bool st) {
  MFCardTypeBtnGroup->blockSignals(true);
  qDebug() << id << MFCardTypeBtnGroup->checkedId();
  if (!st) {
    int result;
    if (id > MFCardTypeBtnGroup
                 ->checkedId()) // id is specified in uiInit() with a proper
                                // order, so I can compare the size by id.
    {
      result = QMessageBox::question(
          this, tr("Info"),
          tr("Some of the data and key will be cleared.") + "\n" +
              tr("Continue?"),
          QMessageBox::Yes | QMessageBox::No);
    } else {
      result = QMessageBox::Yes;
    }
    if (result == QMessageBox::Yes) {
      qDebug() << "Yes";
      mifare->setCardType(MFCardTypeBtnGroup->checkedId());
      MF_widgetReset();
      mifare->data_syncWithDataWidget();
      mifare->data_syncWithKeyWidget();
    } else {
      qDebug() << "No";
      MFCardTypeBtnGroup->button(id)->setChecked(true);
    }
  }
  MFCardTypeBtnGroup->blockSignals(false);
}

void MainWindow::on_MF_selectAllBox_stateChanged(int arg1) {
  ui->MF_dataWidget->blockSignals(true);
  ui->MF_selectAllBox->blockSignals(true);
  ui->MF_selectTrailerBox->blockSignals(true);
  if (arg1 == Qt::PartiallyChecked) {
    ui->MF_selectAllBox->setTristate(false);
    ui->MF_selectAllBox->setCheckState(Qt::Checked);
  }
  for (int i = 0; i < mifare->cardType.block_size; i++) {
    ui->MF_dataWidget->item(i, 1)->setCheckState(
        ui->MF_selectAllBox->checkState());
  }
  for (int i = 0; i < mifare->cardType.sector_size; i++) {
    ui->MF_dataWidget->item(mifare->cardType.blks[i], 0)
        ->setCheckState(ui->MF_selectAllBox->checkState());
  }
  ui->MF_selectTrailerBox->setCheckState(ui->MF_selectAllBox->checkState());
  ui->MF_dataWidget->blockSignals(false);
  ui->MF_selectAllBox->blockSignals(false);
  ui->MF_selectTrailerBox->blockSignals(false);
}

void MainWindow::on_MF_selectTrailerBox_stateChanged(int arg1) {
  int selectedSubBlocks = 0;

  ui->MF_dataWidget->blockSignals(true);
  ui->MF_selectAllBox->blockSignals(true);
  ui->MF_selectTrailerBox->blockSignals(true);
  if (arg1 == Qt::PartiallyChecked) {
    ui->MF_selectTrailerBox->setTristate(false);
    ui->MF_selectTrailerBox->setCheckState(Qt::Checked);
  }
  for (int i = 0; i < mifare->cardType.sector_size; i++) {
    ui->MF_dataWidget
        ->item(mifare->cardType.blks[i] + mifare->cardType.blk[i] - 1, 1)
        ->setCheckState(ui->MF_selectTrailerBox->checkState());
    selectedSubBlocks = 0;
    for (int j = 0; j < mifare->cardType.blk[i]; j++) {
      if (ui->MF_dataWidget->item(j + mifare->cardType.blks[i], 1)
              ->checkState() == Qt::Checked)
        selectedSubBlocks++;
    }
    if (selectedSubBlocks == 0) {
      ui->MF_dataWidget->item(mifare->cardType.blks[i], 0)
          ->setCheckState(Qt::Unchecked);
    } else if (selectedSubBlocks == mifare->cardType.blk[i]) {
      ui->MF_dataWidget->item(mifare->cardType.blks[i], 0)
          ->setCheckState(Qt::Checked);
    } else {
      ui->MF_dataWidget->item(mifare->cardType.blks[i], 0)
          ->setCheckState(Qt::PartiallyChecked);
    }
  }

  ui->MF_dataWidget->blockSignals(false);
  ui->MF_selectAllBox->blockSignals(false);
  ui->MF_selectTrailerBox->blockSignals(false);
}

void MainWindow::on_MF_data2KeyButton_clicked() { mifare->data_data2Key(); }

void MainWindow::on_MF_key2DataButton_clicked() { mifare->data_key2Data(); }

void MainWindow::on_MF_fillKeysButton_clicked() { mifare->data_fillKeys(); }

void MainWindow::on_MF_trailerDecoderButton_clicked() {
  decDialog = new MF_trailerDecoderDialog(this);
  decDialog->show();
}

void MainWindow::on_MF_dataWidget_itemChanged(QTableWidgetItem *item) {
  ui->MF_dataWidget->blockSignals(true);
  ui->MF_selectAllBox->blockSignals(true);
  ui->MF_selectTrailerBox->blockSignals(true);
  if (item->column() == 0) {
    int selectedSectors = 0;
    for (int i = 0; i < mifare->cardType.blk[Mifare::data_b2s(item->row())];
         i++) {
      ui->MF_dataWidget->item(i + item->row(), 1)
          ->setCheckState(item->checkState());
      qDebug() << i << mifare->cardType.blk[item->row()] << i + item->row()
               << ui->MF_dataWidget->item(i + item->row(), 1)->text();
    }
    for (int i = 0; i < mifare->cardType.sector_size; i++) {
      if (ui->MF_dataWidget->item(mifare->cardType.blks[i], 0)->checkState() ==
          Qt::Checked) {
        selectedSectors++;
      }
    }
    if (selectedSectors == 0) {
      ui->MF_selectAllBox->setCheckState(Qt::Unchecked);
      ui->MF_selectTrailerBox->setCheckState(Qt::Unchecked);
    } else if (selectedSectors == mifare->cardType.sector_size) {
      ui->MF_selectAllBox->setCheckState(Qt::Checked);
      ui->MF_selectTrailerBox->setCheckState(Qt::Checked);
    } else {
      ui->MF_selectAllBox->setCheckState(Qt::PartiallyChecked);
      ui->MF_selectTrailerBox->setCheckState(Qt::PartiallyChecked);
    }
  } else if (item->column() == 1) {
    int selectedSubBlocks = 0;
    int selectedBlocks = 0;
    int selectedTrailers = 0;

    for (int i = 0; i < mifare->cardType.block_size; i++) {
      if (ui->MF_dataWidget->item(i, 1)->checkState() == Qt::Checked)
        selectedBlocks++;
    }
    for (int i = 0; i < mifare->cardType.blk[Mifare::data_b2s(item->row())];
         i++) {
      if (ui->MF_dataWidget
              ->item(i + mifare->cardType.blks[Mifare::data_b2s(item->row())],
                     1)
              ->checkState() == Qt::Checked)
        selectedSubBlocks++;
    }
    for (int i = 0; i < mifare->cardType.sector_size; i++) {
      int targetBlock = mifare->cardType.blks[i] + mifare->cardType.blk[i] - 1;
      if (ui->MF_dataWidget->item(targetBlock, 1)->checkState() == Qt::Checked)
        selectedTrailers++;
    }
    if (selectedBlocks == 0) {
      ui->MF_selectAllBox->setCheckState(Qt::Unchecked);
    } else if (selectedBlocks == mifare->cardType.block_size) {
      ui->MF_selectAllBox->setCheckState(Qt::Checked);
    } else {
      ui->MF_selectAllBox->setCheckState(Qt::PartiallyChecked);
    }
    if (selectedSubBlocks == 0) {
      ui->MF_dataWidget
          ->item(mifare->cardType.blks[Mifare::data_b2s(item->row())], 0)
          ->setCheckState(Qt::Unchecked);
    } else if (selectedSubBlocks ==
               mifare->cardType.blk[Mifare::data_b2s(item->row())]) {
      ui->MF_dataWidget
          ->item(mifare->cardType.blks[Mifare::data_b2s(item->row())], 0)
          ->setCheckState(Qt::Checked);
    } else {
      ui->MF_dataWidget
          ->item(mifare->cardType.blks[Mifare::data_b2s(item->row())], 0)
          ->setCheckState(Qt::PartiallyChecked);
    }
    if (selectedTrailers == 0) {
      ui->MF_selectTrailerBox->setCheckState(Qt::Unchecked);
    } else if (selectedTrailers == mifare->cardType.sector_size) {
      ui->MF_selectTrailerBox->setCheckState(Qt::Checked);
    } else {
      ui->MF_selectTrailerBox->setCheckState(Qt::PartiallyChecked);
    }
  } else if (item->column() == 2) {
    QString data = item->text().remove(" ").toUpper();
    if (data == "" || mifare->data_isDataValid(data) == Mifare::DATA_NOSPACE) {
      mifare->data_setData(item->row(), data);
    } else {
      QMessageBox::information(
          this, tr("Info"),
          tr("Data must consists of 32 Hex symbols(Whitespace is allowed)"));
    }
    mifare->data_syncWithDataWidget(false, item->row());
  }
  ui->MF_dataWidget->blockSignals(false);
  ui->MF_selectAllBox->blockSignals(false);
  ui->MF_selectTrailerBox->blockSignals(false);
}

void MainWindow::on_MF_keyWidget_itemChanged(QTableWidgetItem *item) {
  if (item->column() == 1) {
    QString key = item->text().remove(" ").toUpper();
    if (key == "" || mifare->data_isKeyValid(key)) {
      mifare->data_setKey(item->row(), Mifare::KEY_A, key);
    } else {
      QMessageBox::information(
          this, tr("Info"),
          tr("Key must consists of 12 Hex symbols(Whitespace is allowed)"));
    }
    mifare->data_syncWithKeyWidget(false, item->row(), Mifare::KEY_A);
  } else if (item->column() == 2) {
    QString key = item->text().remove(" ").toUpper();
    if (key == "" || mifare->data_isKeyValid(key)) {
      mifare->data_setKey(item->row(), Mifare::KEY_B, key);
    } else {
      QMessageBox::information(
          this, tr("Info"),
          tr("Key must consists of 12 Hex symbols(Whitespace is allowed)"));
    }
    mifare->data_syncWithKeyWidget(false, item->row(), Mifare::KEY_B);
  }
}

void MainWindow::on_MF_File_loadButton_clicked() {
  QString title = "";
  QString filename = "";
  if (ui->MF_File_dataButton->isChecked()) {
    title = tr("Plz select the data file:");
    filename = QFileDialog::getOpenFileName(
        this, title, "./",
        tr("Binary Data Files(*.bin *.dump)") + ";;" +
            tr("Text Data Files(*.txt *.eml)") + ";;" + tr("All Files(*.*)"));
    qDebug() << filename;
    if (filename != "") {
      if (!mifare->data_loadDataFile(filename)) {
        QMessageBox::information(this, tr("Info"),
                                 tr("Failed to open") + "\n" + filename);
      }
    }
  } else if (ui->MF_File_keyButton->isChecked()) {
    title = tr("Plz select the key file:");
    filename = QFileDialog::getOpenFileName(
        this, title, "./",
        tr("Binary Key Files(*.bin *.dump)") + ";;" + tr("All Files(*.*)"));
    qDebug() << filename;
    if (filename != "") {
      if (!mifare->data_loadKeyFile(filename)) {
        QMessageBox::information(this, tr("Info"),
                                 tr("Failed to open") + "\n" + filename);
      }
    }
  }
}

void MainWindow::on_MF_File_saveButton_clicked() {

  QString title = "";
  QString filename = "";
  QString selectedType = "";
  QString defaultName = mifare->data_getUID();
  if (defaultName != "")
    defaultName += "_";
  defaultName += QDateTime::currentDateTime().toString("yyyy-MM-dd-hh-mm-ss");

  if (ui->MF_File_dataButton->isChecked()) {
    title = tr("Plz select the location to save data file:");
    filename = QFileDialog::getSaveFileName(
        this, title, "./data_" + defaultName,
        tr("Binary Data Files(*.bin *.dump)") + ";;" +
            tr("Text Data Files(*.txt *.eml)"),
        &selectedType);
    qDebug() << filename;
    if (filename != "") {
      if (!mifare->data_saveDataFile(
              filename,
              selectedType == tr("Binary Data Files(*.bin *.dump)"))) {
        QMessageBox::information(this, tr("Info"),
                                 tr("Failed to save to") + "\n" + filename);
      }
    }
  } else if (ui->MF_File_keyButton->isChecked()) {
    title = tr("Plz select the location to save key file:");
    filename = QFileDialog::getSaveFileName(
        this, title, "./key_" + defaultName,
        tr("Binary Key Files(*.bin *.dump)"), &selectedType);
    qDebug() << filename;
    if (filename != "") {
      if (!mifare->data_saveKeyFile(
              filename, selectedType == tr("Binary Key Files(*.bin *.dump)"))) {
        QMessageBox::information(this, tr("Info"),
                                 tr("Failed to save to") + "\n" + filename);
      }
    }
  }
  qDebug() << filename << selectedType;
}

void MainWindow::on_MF_File_clearButton_clicked() {
  if (ui->MF_File_keyButton->isChecked()) {
    mifare->data_clearKey();
    mifare->data_syncWithKeyWidget();
  } else if (ui->MF_File_dataButton->isChecked()) {
    mifare->data_clearData();
    mifare->data_syncWithDataWidget();
  }
}

void MainWindow::on_MF_Attack_infoButton_clicked() { mifare->info(); }

// 有bug
// void MainWindow::on_MF_Attack_infoButton_clicked() {
//     setState(false); // 禁用界面，防止乱点

//     // 强制发送完整的 hf mf info 命令，并等待最长 3000 毫秒（因为漏洞检测耗时较长）
//     QString result = util->execCMDWithOutput(
//         "hf mf info",
//         Util::ReturnTrigger(6000, {"Prng", "Hardened", "Can't found", "No valid", "fm11rf08s"})
//         );

//     setState(true);  // 恢复界面

//     // 检查是否成功读取
//     if (result.isEmpty() || result.contains("Can't found", Qt::CaseInsensitive) || result.contains("No valid", Qt::CaseInsensitive)) {
//         QMessageBox::warning(this, tr("读取失败"), tr("未检测到卡片，请调整卡片位置后重试！"));
//         return;
//     }

//     // 1. 提取基本信息 (UID, ATQA, SAK)
//     QString uid = "-", atqa = "-", sak = "-";
//     QStringList lines = result.split("\n");
//     for (const QString &line : lines) {
//         if (line.contains("UID")) uid = QString(line).remove("UID").remove(QRegularExpression("[^0-9a-fA-F]")).trimmed();
//         else if (line.contains("ATQA")) atqa = QString(line).remove("ATQA").remove(QRegularExpression("[^0-9a-fA-F]")).trimmed();
//         else if (line.contains("SAK")) sak = QString(line).remove("SAK").remove(QRegularExpression("\\[.+?\\]")).remove(QRegularExpression("[^0-9a-fA-F]")).trimmed();
//     }

//     // 2. 融合你的“智能破解建议”逻辑 (直接从 result 中解析，不再依赖控制台延迟触发)
//     QString vulnHint = tr("👉 建议步骤：请点击上方【(2)扫描默认密码】(Chk) 碰碰运气。"); // 默认兜底建议

//     if (result.contains("Hint: Try `script run fm11rf08s_recovery.py", Qt::CaseInsensitive)) {
//         vulnHint = tr("【检测到复旦三代无漏洞卡 (FM11RF08S)】\n👉 建议步骤：直接点击【解三代卡】运行脚本。");
//     } else if (result.contains("[+] Static nonce... yes", Qt::CaseInsensitive) || result.contains("[+] Static enc nonce... yes", Qt::CaseInsensitive)) {
//         vulnHint = tr("【检测到静态随机数 (Static Nonce) 漏洞】\n👉 操作建议：先点击【(2)扫描默认密码】，再点击【知一求全】极速秒解。");
//     } else if (result.contains("[+] Prng....... weak", Qt::CaseInsensitive)) {
//         vulnHint = tr("【检测到弱随机数 (Weak PRNG) 漏洞】\n👉 操作建议：先点击【(2)扫描默认密码】，再点击【知一求全】常规破解。");
//     } else if (result.contains("Hardened MIFARE Classic", Qt::CaseInsensitive)) {
//         vulnHint = tr("【检测到强化加密 (Hardened) 卡片】\n👉 操作建议：先点击【(2)扫描默认密码】，如果扫到密码，再使用【Hardnested攻击】。");
//     }

//     // 3. 拼接终极弹窗展示
//     QString msg = tr("读取卡片信息成功！\n\n"
//                      "卡号 (UID): %1\n"
//                      "厂商 (ATQA): %2\n"
//                      "类型 (SAK): %3\n\n"
//                      "------------------------\n"
//                      "%4")
//                       .arg(uid, atqa, sak, vulnHint);

//     QMessageBox::information(this, tr("第一步：卡片状态与破解诊断"), msg);

//     // 4. (可选) 将完整的输出同步显示在底部的 Raw 控制台中，方便高级用户看日志
//     refreshCMD("hf mf info");
//     refreshOutput(result);
// }

void MainWindow::on_MF_Attack_chkButton_clicked() {
  setState(false);
  mifare->chk();
  setState(true);
}

void MainWindow::on_MF_Attack_nestedButton_clicked() {
  setState(false);
  mifare->nested();
  setState(true);
}

void MainWindow::on_MF_Attack_hardnestedButton_clicked() {
  mifare->hardnested();
}

void MainWindow::on_MF_RW_readSelectedButton_clicked() {
  setState(false);
  mifare->readSelected(Mifare::TARGET_MIFARE);
  setState(true);
}

void MainWindow::on_MF_RW_readBlockButton_clicked() {
    setState(false);
    mifare->readOne(Mifare::TARGET_MIFARE);
    setState(true);

    // === 新增：如果读取的是 0 块且读取成功，给出防砖提示 ===
    if (ui->MF_RW_blockBox->currentText() == "0") {
        QString data = ui->MF_RW_dataEdit->text();
        // 如果数据不为空，且没有包含 "Failed!"（代表读取成功了）
        if (!data.isEmpty() && !data.contains("Failed!", Qt::CaseInsensitive)) {
            QMessageBox::information(this, tr("防砖提示"),
                                     tr("您刚刚读取了第 0 块（包含卡号和厂商信息）。\n\n"
                                        "⚠️ 警告：请不要直接在下方的数据框中手动修改！\n"
                                        "手动修改极易导致 BCC 校验和错误，从而使卡片永久变砖。\n\n"
                                        "👉 建议操作：请点击右侧的【修改卡号】按钮，使用专用的修改器来安全改写卡号。"));
        }
    }
}

void MainWindow::on_MF_RW_writeBlockButton_clicked() {
  setState(false);
  mifare->writeOne();
  setState(true);
}

void MainWindow::on_MF_RW_writeSelectedButton_clicked() {
  setState(false);
  mifare->writeSelected(Mifare::TARGET_MIFARE);
  setState(true);
}

void MainWindow::on_MF_RW_dumpButton_clicked() {
    // 尝试从左侧数据面板获取卡号 (UID)
    QString uid = mifare->data_getUID();
    QString baseName;

    // 判断是否获取到了有效的 UID
    if (!uid.isEmpty() && !uid.contains("?") && uid != "00000000") {
        baseName = QString("hf-mf-%1").arg(uid.toUpper());
    } else {
        baseName = "hf-mf-unknown";
    }

    // 默认第一个文件名
    QString keyFileName = baseName + "-key.bin";
    QString keyFilePath = QDir::homePath() + "/" + keyFileName;

    // --- 核心优化：模仿官方，重名则自动追加 -001, -002 ---
    int counter = 1;
    while (QFile::exists(keyFilePath)) {
        // arg(counter, 3, 10, QChar('0')) 的作用是将数字补齐为 3 位，如 1 变成 001
        keyFileName = baseName + QString("-key-%1.bin").arg(counter, 3, 10, QChar('0'));
        keyFilePath = QDir::homePath() + "/" + keyFileName;
        counter++;
    }
    // --------------------------------------------------

    // 自动将右侧面板当前的密钥保存为不重名的二进制 Key 文件
    if (mifare->data_saveKeyFile(keyFilePath, true)) {
        // 带着新生成的密钥文件 (例如 hf-mf-UID-key-002.bin) 去执行 Dump
        mifare->dump(keyFileName);
    } else {
        // 如果保存失败，退回默认的无参 Dump
        mifare->dump();
    }
}

void MainWindow::on_MF_RW_restoreButton_clicked() {
    QString dumpFilename = "";
    QString keyFilename = "";

    // 0. 智能嗅探：从左侧数据面板中获取当前加载的 0 块 UID
    QString uid = mifare->data_getUID();

    if (!uid.isEmpty() && uid != "00000000" && uid != "FFFFFFFF") {
        QStringList searchPaths;
        searchPaths << clientWorkingDir->absolutePath() << QDir::homePath();

        QString autoDumpPath = "";
        QString autoKeyPath = "";

        QString uidLower = uid.toLower();
        QString uidUpper = uid.toUpper();

        for (const QString &path : searchPaths) {
            QDir dir(path);
            if (!dir.exists()) continue;

            QStringList dumpFilters;
            dumpFilters << "*" + uidLower + "*dump*.bin" << "*" + uidUpper + "*dump*.bin";
            QStringList dumpFiles = dir.entryList(dumpFilters, QDir::Files, QDir::Time);

            QStringList keyFilters;
            keyFilters << "*" + uidLower + "*key*.bin" << "*" + uidUpper + "*key*.bin";
            QStringList keyFiles = dir.entryList(keyFilters, QDir::Files, QDir::Time);

            if (!dumpFiles.isEmpty()) {
                autoDumpPath = dir.absoluteFilePath(dumpFiles.first());
                if (!keyFiles.isEmpty()) {
                    autoKeyPath = dir.absoluteFilePath(keyFiles.first());
                }
                break;
            }
        }

        if (!autoDumpPath.isEmpty()) {
            QString msg = tr("检测到面板加载数据对应的备份文件 (卡号 %1)：\n\n"
                             "📄 %2\n").arg(uid, autoDumpPath);
            if (!autoKeyPath.isEmpty()) {
                msg += tr("🔑 %1\n").arg(autoKeyPath);
            }
            msg += tr("\n是否将这些文件写入到当前放着的卡片中？");

            QMessageBox msgBox(this);
            msgBox.setWindowTitle(tr("智能发现匹配文件"));
            msgBox.setText(msg);
            msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            msgBox.setDefaultButton(QMessageBox::Yes);

            int reply = msgBox.exec();
            if (reply == QMessageBox::Cancel) {
                return; // 拦截：点击取消或右上角的 X
            } else if (reply == QMessageBox::Yes) {
                dumpFilename = autoDumpPath;
                keyFilename = autoKeyPath;
            }
        } else {
            QMessageBox::information(this, tr("智能匹配失败"),
                                     tr("在工作目录和用户根目录下均未找到卡号包含 [%1] 的 dump 文件。\n"
                                        "请手动选择要写入的数据文件。").arg(uid));
        }
    } else {
        QMessageBox::information(this, tr("面板无数据"),
                                 tr("左侧数据面板为空，或者尚未加载需要写入的扇区数据。\n"
                                    "将进入手动选择文件模式。"));
    }

    // 1. 手动降级 Dump
    if (dumpFilename.isEmpty()) {
        QString dumpTitle = tr("第一步：请手动选择数据文件 (Dump.bin)");
        dumpFilename = QFileDialog::getOpenFileName(
            this, dumpTitle, QDir::homePath(),
            tr("Binary Data Files(*.bin *.dump)") + ";;" + tr("All Files(*.*)"));
        if (dumpFilename.isEmpty()) return; // 拦截：取消选择则退出
    }

    // 2. 询问目标卡物理状态 (这决定了是否给 PM3 下达 --ka 指令)
    QMessageBox cardStateBox(this);
    cardStateBox.setWindowTitle(tr("第二步：目标卡状态确认"));
    cardStateBox.setText(tr("您要写入的【目标卡】，当前是【全新空白卡】还是【已有密码的加密卡】？\n\n"
                            "👉 白卡：正常写入 (不加 --ka)。\n"
                            "👉 加密卡：强制使用旧密码验证写入 (增加 --ka)。\n\n"
                            "⚠️ 提示：无论您选择哪种卡，下一步都必须提供与 Dump 配套的 Key 密钥文件！"));
    QPushButton *blankBtn = cardStateBox.addButton(tr("空白卡 (不加 --ka)"), QMessageBox::ActionRole);
    cardStateBox.addButton(tr("加密卡 (加 --ka)"), QMessageBox::ActionRole);
    cardStateBox.addButton(QMessageBox::Cancel);

    cardStateBox.exec();

    if (cardStateBox.clickedButton() == cardStateBox.button(QMessageBox::Cancel) || cardStateBox.clickedButton() == 0) {
        return;
    }

    // true 表示不加 --ka, false 表示加 --ka
    bool isBlankCard = (cardStateBox.clickedButton() == blankBtn);

    // 3. 处理 Key 文件逻辑 (【关键修复】无论白卡黑卡，如果没找到 Key，都必须让用户手动选！)
    if (keyFilename.isEmpty()) {
        QString keyTitle = tr("第三步：请手动选择对应的密钥文件 (Key.bin)");
        keyFilename = QFileDialog::getOpenFileName(
            this, keyTitle, QDir::homePath(),
            tr("Binary Key Files(*.bin *.dump *.key)") + ";;" + tr("All Files(*.*)"));
        if (keyFilename.isEmpty()) return; // 拦截：取消选择则退出
    }

    // 4. 询问强制覆盖
    QMessageBox forceBox(this);
    forceBox.setWindowTitle(tr("第四步：附加选项"));
    forceBox.setText(tr("是否启用 --force 强制覆盖？\n\n"
                        "👉 强制：忽略卡号(UID)不匹配警告，强行覆盖写入。\n"
                        "👉 不强制：遇到 UID 不匹配会安全中断，保护卡片。"));
    QPushButton *forceBtn = forceBox.addButton(tr("强制覆盖 (--force)"), QMessageBox::ActionRole);
    forceBox.addButton(tr("安全写入 (不强制)"), QMessageBox::ActionRole); // <-- 这里去掉了未使用的变量声明
    forceBox.addButton(QMessageBox::Cancel);

    forceBox.exec();

    if (forceBox.clickedButton() == forceBox.button(QMessageBox::Cancel) || forceBox.clickedButton() == 0) {
        return;
    }

    bool force = (forceBox.clickedButton() == forceBtn);

    // 5. 智能融合逻辑 (增强版：直接修改 UI 控件)
    mifare->data_loadDataFile(dumpFilename);
    mifare->data_loadKeyFile(keyFilename);

    int sectors = mifare->getCardType().sector_size;
    for (int i = 0; i < sectors; i++) {
        int trailerBlk = mifare->getTrailerBlockId(i);
        QTableWidgetItem *dataItem = ui->MF_dataWidget->item(trailerBlk, 2);
        if (!dataItem) continue;

        QString trailerData = dataItem->text().remove(" ").toUpper();

        // 获取 Key 列表中的真实密码
        QTableWidgetItem *keyAItem = ui->MF_keyWidget->item(i, 1);
        QTableWidgetItem *keyBItem = ui->MF_keyWidget->item(i, 2);
        QString keyA = keyAItem ? keyAItem->text().remove(" ").toUpper() : "FFFFFFFFFFFF";
        QString keyB = keyBItem ? keyBItem->text().remove(" ").toUpper() : "FFFFFFFFFFFF";

        if (trailerData.length() == 32) {
            QString dumpKeyB = trailerData.right(12);

            // 【核心策略】：如果 dump 里是 0，且 Key 列表里有数据，则强制修补
            if (dumpKeyB == "000000000000" || dumpKeyB == "FFFFFFFFFFFF") {
                if (keyB != "000000000000" && keyB != "FFFFFFFFFFFF") {
                    trailerData.replace(20, 12, keyB);
                    // 关键点：不仅修补内存，还要同步回 UI 界面，确保导出的是正确的
                    QString formatted;
                    for (int i = 0; i < trailerData.length(); i += 2) {
                        formatted += trailerData.mid(i, 2) + " ";
                    }
                    dataItem->setText(formatted.trimmed());
                }
            }
        }
    }

    // 强制同步：把 UI 的修改同步到 mifare 对象的内部缓存
    mifare->data_syncWithDataWidget(false, 0);

    // 保存修补后的文件
    QString patchedDump = clientWorkingDir->absolutePath() + "/restore_patched_dump.bin";
    if (mifare->data_saveDataFile(patchedDump, true)) {
        // 6. 执行写入
        mifare->restore(patchedDump, keyFilename, isBlankCard, force);

        // 【贴心小提示】
        QMessageBox::information(this, tr("写入完成"),
                                 tr("数据已尝试修补并下发指令给 PM3。\n\n"
                                    "⚠️ 重要提示：\n"
                                    "由于 PM3 软件缺陷，再次执行 [Autopwn/破解] 会强制在日志里把 KeyB 显示为 0。\n"
                                    "请直接使用 [Read Block] 或刷卡测试来验证结果！"));
    }
}

void MainWindow::on_MF_UID_readSelectedButton_clicked() {
  setState(false);
  mifare->readSelected(Mifare::TARGET_UID);
  setState(true);
}

void MainWindow::on_MF_UID_readBlockButton_clicked() {
  setState(false);
  mifare->readOne(Mifare::TARGET_UID);
  setState(true);
}

void MainWindow::on_MF_UID_writeSelectedButton_clicked() {
  setState(false);
  mifare->writeSelected(Mifare::TARGET_UID);
  setState(true);
}

void MainWindow::on_MF_UID_writeBlockButton_clicked() {
  setState(false);
  mifare->writeOne(Mifare::TARGET_UID);
  setState(true);
}

void MainWindow::on_MF_UID_wipeButton_clicked() { mifare->wipeC(); }

void MainWindow::on_MF_UID_aboutUIDButton_clicked() {
  QString msg;
  msg +=
      tr("    Normally, the Block 0 of a typical Mifare card, which contains "
         "the UID, is locked during the manufacture. Users cannot write "
         "anything to Block 0 or set a new UID to a normal Mifare card.") +
      "\n";
  msg +=
      tr("    Chinese Magic Cards(aka UID Cards) are some special cards whose "
         "Block 0 are writeable. And you can change UID by writing to it.") +
      "\n";
  msg += "\n";
  msg += tr("There are two versions of Chinese Magic Cards, the Gen1 and the "
            "Gen2.") +
         "\n";
  msg += tr("    Gen1:") + "\n" +
         tr("    also called UID card in China. It responses to some backdoor "
            "commands so you can access any blocks without password. The "
            "Proxmark3 has a bunch of related commands(csetblk, cgetblk, ...) "
            "to deal with this type of card, and my GUI also support these "
            "commands.") +
         "\n";
  msg += tr("    Gen2:") + "\n" +
         tr("    doesn't response to the backdoor commands, which means that a "
            "reader cannot detect whether it is a Chinese Magic Card or not by "
            "sending backdoor commands.") +
         "\n";
  msg += "\n";
  msg += tr("There are some types of Chinese Magic Card Gen2.") + "\n";
  msg += tr("    CUID Card:") + "\n" +
         tr("    the Block 0 is writeable, you can write to this block "
            "repeatedly by normal wrbl command.") +
         "\n";
  msg += tr("    (hf mf wrbl 0 A FFFFFFFFFFFF <the data you want to write>)") +
         "\n";
  msg += tr("    FUID Card:") + "\n" +
         tr("    you can only write to Block 0 once. After that, it seems like "
            "a typical Mifare card(Block 0 cannot be written to).") +
         "\n";
  msg += tr("    (some readers might try changing the Block 0, which could "
            "detect the CUID Card. In that case, you should use FUID card.)") +
         "\n";
  msg += tr("    UFUID Card:") + "\n" +
         tr("    It behaves like a CUID card(or UID card? I'm not sure) before "
            "you send some special command to lock it. Once it is locked, you "
            "cannot change its Block 0(just like a typical Mifare card).") +
         "\n";
  msg += "\n";
  msg += tr("    Seemingly, these Chinese Magic Cards are more easily to be "
            "compromised by Nested Attack(it takes little time to get an "
            "unknown key).") +
         "\n";
  QMessageBox::information(this, tr("About UID Card"), msg);
}

void MainWindow::on_MF_UID_setParaButton_clicked() {
  setState(false);
  mifare->setParameterC();
  setState(true);
}

void MainWindow::on_MF_UID_lockButton_clicked() { mifare->lockC(); }

void MainWindow::on_MF_Sim_readSelectedButton_clicked() {
  setState(false);
  mifare->readSelected(Mifare::TARGET_EMULATOR);
  setState(true);
}

void MainWindow::on_MF_Sim_writeSelectedButton_clicked() {
  setState(false);
  mifare->writeSelected(Mifare::TARGET_EMULATOR);
  setState(true);
}

void MainWindow::on_MF_Sim_clearButton_clicked() { mifare->wipeE(); }

void MainWindow::on_MF_Sim_simButton_clicked() { mifare->simulate(); }

void MainWindow::on_MF_Sniff_loadButton_clicked() // use a tmp file to support
                                                  // complicated path
{
  QString title = "";
  QString filename = "";
  QString defaultExtension;
  QDir clientTracePath;

  if (Util::getClientType() == Util::CLIENTTYPE_OFFICIAL)
    defaultExtension = ".trc";
  else if (Util::getClientType() == Util::CLIENTTYPE_ICEMAN)
    defaultExtension = ".trace";

  QString userTraceSavePath = mifare->getTraceSavePath();
  if (userTraceSavePath.isEmpty())
    clientTracePath = *clientWorkingDir;
  else
    clientTracePath = QDir(userTraceSavePath); // For v4.16717 and later

  title = tr("Plz select the trace file:");
  filename =
      QFileDialog::getOpenFileName(this, title, clientTracePath.absolutePath(),
                                   tr("Trace Files") + "(*" + defaultExtension +
                                       ")" + ";;" + tr("All Files(*.*)"));
  qDebug() << filename;
  if (filename != "") {
    QString tmpFile =
        "tmp" + QString::number(QDateTime::currentDateTimeUtc().toTime_t()) +
        defaultExtension;
    if (QFile::copy(filename, clientTracePath.absolutePath() + "/" + tmpFile)) {
      mifare->loadSniff(tmpFile);
      util->delay(3000);
      QFile::remove(clientTracePath.absolutePath() + "/" + tmpFile);
    } else {
      QMessageBox::information(this, tr("Info"),
                               tr("Failed to open") + "\n" + filename);
    }
  }
}

void MainWindow::on_MF_Sniff_saveButton_clicked() {
  QString title = "";
  QString filename = "";
  QString defaultExtension;
  QDir clientTracePath;

  if (Util::getClientType() == Util::CLIENTTYPE_OFFICIAL)
    defaultExtension = ".trc";
  else if (Util::getClientType() == Util::CLIENTTYPE_ICEMAN)
    defaultExtension = ".trace";

  QString userTraceSavePath = mifare->getTraceSavePath();
  if (userTraceSavePath.isEmpty())
    clientTracePath = *clientWorkingDir;
  else
    clientTracePath = QDir(userTraceSavePath); // For v4.16717 and later

  title = tr("Plz select the location to save trace file:");
  filename = QFileDialog::getSaveFileName(
      this, title, clientTracePath.absolutePath(),
      tr("Trace Files") + "(*" + defaultExtension + ")");
  qDebug() << filename;
  if (filename != "") {
    QString tmpFile =
        "tmp" + QString::number(QDateTime::currentDateTimeUtc().toTime_t()) +
        defaultExtension;
    mifare->saveSniff(tmpFile);
    for (int i = 0; i < 100; i++) {
      util->delay(100);
      if (QFile::exists(clientTracePath.absolutePath() + "/" + tmpFile))
        break;
    }
    // filename is not empty -> the user has chosen to overwrite the existing
    // file
    if (QFile::exists(filename))
      QFile::remove(filename);
    if (!QFile::copy(clientTracePath.absolutePath() + "/" + tmpFile,
                     filename)) {
      QMessageBox::information(this, tr("Info"),
                               tr("Failed to save to") + "\n" + filename);
    }
    QFile::remove(clientTracePath.absolutePath() + "/" + tmpFile);
  }
}

void MainWindow::on_MF_Sniff_sniffButton_clicked() {
  setState(false);
  mifare->sniff();
  setState(true);
}

void MainWindow::on_MF_14aSniff_snoopButton_clicked() {
  setState(false);
  mifare->sniff14a();
  setState(true);
}

void MainWindow::on_MF_Sniff_listButton_clicked() { mifare->list(); }

void MainWindow::MF_widgetReset() {
  int secs = mifare->cardType.sector_size;
  int blks = mifare->cardType.block_size;
  QBrush trailerItemForeColor = QBrush(QColor(0, 160, 255));
  ui->MF_RW_blockBox->clear();
  ui->MF_keyWidget->setRowCount(secs);
  ui->MF_dataWidget->setRowCount(blks);

  ui->MF_dataWidget->blockSignals(true);
  ui->MF_keyWidget->blockSignals(true);
  ui->MF_selectAllBox->blockSignals(true);
  ui->MF_selectTrailerBox->blockSignals(true);

  for (int i = 0; i < blks; i++) {
    setTableItem(ui->MF_dataWidget, i, 0, "");
    setTableItem(ui->MF_dataWidget, i, 1, QString::number(i));
    ui->MF_dataWidget->item(i, 1)->setCheckState(Qt::Checked);
    setTableItem(ui->MF_dataWidget, i, 2, "");
    ui->MF_RW_blockBox->addItem(QString::number(i));
  }

  for (int i = 0; i < secs; i++) {
    setTableItem(ui->MF_keyWidget, i, 0, QString::number(i));
    setTableItem(ui->MF_keyWidget, i, 1, "");
    setTableItem(ui->MF_keyWidget, i, 2, "");
    setTableItem(ui->MF_dataWidget, mifare->cardType.blks[i], 0,
                 QString::number(i));
    ui->MF_dataWidget
        ->item(mifare->cardType.blks[i] + mifare->cardType.blk[i] - 1, 2)
        ->setForeground(trailerItemForeColor);
    ui->MF_dataWidget->item(mifare->cardType.blks[i], 0)
        ->setCheckState(Qt::Checked);
  }
  ui->MF_dataWidget->item(0, 2)->setForeground(QBrush(QColor(255, 160, 0)));
  ui->MF_selectAllBox->setCheckState(Qt::Checked);
  ui->MF_selectTrailerBox->setCheckState(Qt::Checked);

  ui->MF_dataWidget->blockSignals(false);
  ui->MF_keyWidget->blockSignals(false);
  ui->MF_selectAllBox->blockSignals(false);
  ui->MF_selectTrailerBox->blockSignals(false);
}
// ************************************************

// ******************** other ********************

void MainWindow::uiInit() {
  connect(ui->Raw_CMDEdit, &QLineEdit::returnPressed, this,
          &MainWindow::sendMSG);
  ui->Raw_CMDEdit->installEventFilter(keyEventFilter);
  connect(keyEventFilter, &MyEventFilter::eventHappened, this,
          &MainWindow::on_Raw_keyPressed);
  ui->MF_keyWidget->installEventFilter(resizeEventFilter);
  connect(resizeEventFilter, &MyEventFilter::eventHappened, this,
          &MainWindow::on_MF_keyWidget_resized);
  ui->Raw_outputEdit->installEventFilter(keyEventFilter);

  connectStatusBar = new QLabel(this);
  programStatusBar = new QLabel(this);
  PM3VersionBar = new QLabel(this);
  stopButton = new QPushButton(this);
  setStatusBar(connectStatusBar, tr("Not Connected"));
  setStatusBar(programStatusBar, tr("Idle"));
  setStatusBar(PM3VersionBar, "");
  stopButton->setText(tr("Stop"));
  ui->statusbar->addPermanentWidget(PM3VersionBar, 1);
  ui->statusbar->addPermanentWidget(connectStatusBar, 1);
  ui->statusbar->addPermanentWidget(programStatusBar, 1);
  ui->statusbar->addPermanentWidget(stopButton);

  ui->MF_dataWidget->setColumnWidth(0, 55);
  ui->MF_dataWidget->setColumnWidth(1, 55);

  ui->MF_keyWidget->setColumnWidth(0, 45);

  MF_widgetReset();
  MFCardTypeBtnGroup = new QButtonGroup(this);
  MFCardTypeBtnGroup->addButton(ui->MF_Type_miniButton, 0);
  MFCardTypeBtnGroup->addButton(ui->MF_Type_1kButton, 1);
  MFCardTypeBtnGroup->addButton(ui->MF_Type_2kButton, 2);
  MFCardTypeBtnGroup->addButton(ui->MF_Type_4kButton, 4);
  connect(MFCardTypeBtnGroup,
          QOverload<int, bool>::of(&QButtonGroup::buttonToggled), this,
          &MainWindow::MF_onMFCardTypeChanged);

  ui->MF_keyWidget->installEventFilter(this);
  ui->MF_dataWidget->installEventFilter(this);

  ui->Set_UI_Theme_nameBox->addItem(tr("(None)"), "(none)");
  ui->Set_UI_Theme_nameBox->addItem(tr("Dark"), "qdss_dark");
  ui->Set_UI_Theme_nameBox->addItem(tr("Light"), "qdss_light");

  settings->beginGroup("UI_grpbox_preference");

  QStringList boxNames = settings->allKeys();
  QGroupBox *boxptr;
  foreach (QString name, boxNames) {
    boxptr = this->findChild<QGroupBox *>(name);
    if (boxptr == nullptr)
      continue;
    if (settings->value(name, true).toBool()) {
      boxptr->setMaximumHeight(16777215);
      boxptr->setChecked(true);
    } else {
      boxptr->setMaximumHeight(20);
      boxptr->setChecked(false);
    }
  }
  settings->endGroup();

  loadClientPathList();

  ui->Set_Client_GUIWorkingDirLabel->setText(QDir::currentPath());

  settings->beginGroup("Client_Args");
  ui->Set_Client_startArgsEdit->setText(
      settings->value("args", "<port> -f").toString());
  settings->endGroup();

  settings->beginGroup("Client_forceButtonsEnabled");
  keepButtonsEnabled = settings->value("state", false).toBool();
  settings->endGroup();
  ui->Set_Client_forceEnabledBox->setChecked(keepButtonsEnabled);

  // the disconnect detection doesn't work well on Linux/macOS
  // So it should be disabled on these platforms
  // https://github.com/wh201906/Proxmark3GUI/issues/22
  // #22, #26, #40, #41
  settings->beginGroup("Client_keepClientActive");
#ifdef Q_OS_WIN
  keepClientActive = settings->value("state", false).toBool();
#else
  keepClientActive = settings->value("state", true).toBool();
#endif
  settings->endGroup();
  ui->Set_Client_keepClientActiveBox->setChecked(keepClientActive);

  QDir configFiles(":/config/");
  configFiles.setSorting(QDir::Name);
  const QFileInfoList configFileList = configFiles.entryInfoList();
  ui->Set_Client_configFileBox->blockSignals(true);
  for (const auto &file : configFileList) {
    ui->Set_Client_configFileBox->addItem(file.fileName(), file.filePath());
  }

  // Use the last one as the default one
  ui->Set_Client_configFileBox->setCurrentIndex(
      ui->Set_Client_configFileBox->count() - 1);
  ui->Set_Client_configFileBox->addItem(tr("External file"), "(ext)");

  int configId = -1;
  settings->beginGroup("Client_Env");
  ui->Set_Client_envScriptEdit->setText(
      settings->value("scriptPath").toString());
  ui->Set_Client_workingDirEdit->setText(
      settings->value("workingDir", "../data").toString());
  configId =
      ui->Set_Client_configFileBox->findData(settings->value("configFile"));
  ui->Set_Client_configPathEdit->setText(
      settings->value("extConfigFilePath", "config.json").toString());
  settings->endGroup();
  if (configId != -1)
    ui->Set_Client_configFileBox->setCurrentIndex(configId);
  ui->Set_Client_configFileBox->blockSignals(false);
  on_Set_Client_configFileBox_currentIndexChanged(
      ui->Set_Client_configFileBox->currentIndex());

  // setValue() will trigger valueChanged()
  // setValue(settings->value()) will create a nested group
  // call endGroup() before apply the value
  settings->beginGroup("UI");
  int opacity = settings->value("Opacity", 100).toInt();
  int themeId = ui->Set_UI_Theme_nameBox->findData(
      settings->value("Theme_Name", "(none)").toString());
  settings->endGroup();
  ui->Set_UI_Opacity_Box->setValue(opacity);
  ui->Set_UI_Theme_nameBox->setCurrentIndex((themeId == -1) ? 0 : themeId);

  settings->beginGroup("UI");
  // QApplication::font() might return wrong result
  // If fonts are not specified in config file, don't touch them.
  QString tmpFontName;
  int tmpFontSize;
  bool fontValid = false, dataFontValid = false, CMDFontValid = false;
  tmpFontName = settings->value("Font_Name", "").toString();
  tmpFontSize = settings->value("Font_Size", -1).toInt();
  if (!tmpFontName.isEmpty() && tmpFontSize != -1 &&
      tmpFontName == QFont(tmpFontName).family()) {
    ui->Set_UI_Font_nameBox->setCurrentFont(QFont(tmpFontName));
    ui->Set_UI_Font_sizeBox->setValue(tmpFontSize);
    fontValid = true;
  }
  // The default font should be the same as MF_dataWidget's and MF_keyWidget's.
  tmpFontName = settings->value("DataFont_Name", "Consolas").toString();
  tmpFontSize = settings->value("DataFont_Size", 12).toInt();
  if (!tmpFontName.isEmpty() && tmpFontSize != -1 &&
      tmpFontName == QFont(tmpFontName).family()) {
    ui->Set_UI_DataFont_nameBox->setCurrentFont(QFont(tmpFontName));
    ui->Set_UI_DataFont_sizeBox->setValue(tmpFontSize);
    dataFontValid = true;
  }
  tmpFontName = settings->value("CMDFont_Name", "").toString();
  tmpFontSize = settings->value("CMDFont_Size", -1).toInt();
  if (!tmpFontName.isEmpty() && tmpFontSize != -1 &&
      tmpFontName == QFont(tmpFontName).family()) {
    ui->Set_UI_CMDFont_nameBox->setCurrentFont(QFont(tmpFontName));
    ui->Set_UI_CMDFont_sizeBox->setValue(tmpFontSize);
    CMDFontValid = true;
  }
  settings->endGroup();

  if (fontValid)
    on_Set_UI_Font_setButton_clicked();
  if (dataFontValid)
    on_Set_UI_DataFont_setButton_clicked();
  if (CMDFontValid)
    on_Set_UI_CMDFont_setButton_clicked();

  ui->MF_RW_keyTypeBox->addItem("A", Mifare::KEY_A);
  ui->MF_RW_keyTypeBox->addItem("B", Mifare::KEY_B);

  on_Raw_CMDHistoryBox_stateChanged(Qt::Unchecked);
}

void MainWindow::signalInit() {
  connect(pm3, &PM3Process::newOutput, util, &Util::processOutput);
  connect(pm3, &PM3Process::changeClientType, util, &Util::setClientType);
  connect(util, &Util::refreshOutput, this, &MainWindow::refreshOutput);

  connect(this, &MainWindow::connectPM3, pm3, &PM3Process::connectPM3);
  connect(this, &MainWindow::reconnectPM3, pm3, &PM3Process::reconnectPM3);
  connect(pm3, &PM3Process::PM3StatedChanged, this,
          &MainWindow::onPM3StateChanged);
  connect(pm3, &PM3Process::PM3StatedChanged, util, &Util::setRunningState);
  connect(pm3, &PM3Process::errorOccurred, this,
          &MainWindow::onPM3ErrorOccurred);
  connect(pm3, &PM3Process::HWConnectFailed, this,
          &MainWindow::onPM3HWConnectFailed);
  connect(this, &MainWindow::killPM3, pm3, &PM3Process::killPM3);
  connect(this, &MainWindow::setProcEnv, pm3, &PM3Process::setProcEnv);
  connect(this, &MainWindow::setWorkingDir, pm3, &PM3Process::setWorkingDir);
  connect(this, QOverload<bool>::of(&MainWindow::setSerialListener), pm3,
          QOverload<bool>::of(&PM3Process::setSerialListener));
  connect(this,
          QOverload<const QString &, bool>::of(&MainWindow::setSerialListener),
          pm3,
          QOverload<const QString &, bool>::of(&PM3Process::setSerialListener));

  connect(util, &Util::write, pm3, &PM3Process::write);

  connect(ui->MF_typeGroupBox, &QGroupBox::clicked, this,
          &MainWindow::on_GroupBox_clicked);
  connect(ui->MF_fileGroupBox, &QGroupBox::clicked, this,
          &MainWindow::on_GroupBox_clicked);
  connect(ui->MF_RWGroupBox, &QGroupBox::clicked, this,
          &MainWindow::on_GroupBox_clicked);
  connect(ui->MF_normalGroupBox, &QGroupBox::clicked, this,
          &MainWindow::on_GroupBox_clicked);
  connect(ui->MF_UIDGroupBox, &QGroupBox::clicked, this,
          &MainWindow::on_GroupBox_clicked);
  // connect(ui->MF_simGroupBox, &QGroupBox::clicked, this, //删除模拟功能
  //         &MainWindow::on_GroupBox_clicked);

  connect(stopButton, &QPushButton::clicked, this,
          &MainWindow::on_stopButton_clicked);

  connect(ui->Set_UI_Opacity_slider, &QSlider::valueChanged,
          ui->Set_UI_Opacity_Box, &QSpinBox::setValue);
}

void MainWindow::setStatusBar(QLabel *target, const QString &text) {
  if (target == PM3VersionBar)
    target->setText(tr("HW Version:") + text);
  else if (target == connectStatusBar)
    target->setText(tr("PM3:") + text);
  else if (target == programStatusBar)
    target->setText(tr("State:") + text);
}

void MainWindow::setTableItem(QTableWidget *widget, int row, int column,
                              const QString &text) {
  if (widget->item(row, column) == nullptr)
    widget->setItem(row, column, new QTableWidgetItem());
  widget->item(row, column)->setText(text);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) // drag support
{
  if (event->type() == QEvent::DragEnter) {
    QDragEnterEvent *dragEvent = static_cast<QDragEnterEvent *>(event);
    dragEvent->acceptProposedAction();
    return true;
  } else if (event->type() == QEvent::Drop) {
    QDropEvent *dropEvent = static_cast<QDropEvent *>(event);
    if (watched == ui->MF_keyWidget) {
      const QMimeData *mime = dropEvent->mimeData();
      if (mime->hasUrls()) {
        QList<QUrl> urls = mime->urls();
        if (urls.length() == 1) {
          mifare->data_loadKeyFile(urls[0].toLocalFile());
          return true;
        }
      }
    } else if (watched == ui->MF_dataWidget) {
      const QMimeData *mime = dropEvent->mimeData();
      if (mime->hasUrls()) {
        QList<QUrl> urls = mime->urls();
        if (urls.length() == 1) {
          mifare->data_loadDataFile(urls[0].toLocalFile());
          return true;
        }
      }
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setState(bool st) {
  if (!st && pm3state) {
    setStatusBar(programStatusBar, tr("Running"));
  } else {
    setStatusBar(programStatusBar, tr("Idle"));
  }
  setButtonsEnabled(st || keepButtonsEnabled);
}

void MainWindow::setButtonsEnabled(bool st) {
  ui->MF_attackGroupBox->setEnabled(st);
  ui->MF_normalGroupBox->setEnabled(st);
  ui->MF_UIDGroupBox->setEnabled(st);
  // ui->MF_simGroupBox->setEnabled(st);
  ui->Raw_CMDEdit->setEnabled(st);
  ui->Raw_sendCMDButton->setEnabled(st);
  ui->LF_LFconfigGroupBox->setEnabled(st);
  ui->LF_operationGroupBox->setEnabled(st);
}

void MainWindow::on_GroupBox_clicked(bool checked) {
  QGroupBox *box = dynamic_cast<QGroupBox *>(sender());

  settings->beginGroup("UI_grpbox_preference");
  if (checked) {
    box->setMaximumHeight(16777215);
    settings->setValue(box->objectName(), true);
  } else {
    box->setMaximumHeight(20);
    settings->setValue(box->objectName(), false);
  }
  settings->endGroup();
}

void MainWindow::addClientPath(const QString &path) {
  m_clientPathList.removeAll(path);
  m_clientPathList.prepend(path);
  while (m_clientPathList.size() > 32) // the maximum count of path items
    m_clientPathList.removeLast();
  // sync to the storage
  saveClientPathList();
  // sync to the UI
  loadClientPathList();
}

void MainWindow::loadClientPathList() {
  m_clientPathList.clear();
  settings->beginGroup("Client_Path");
  int len = settings->beginReadArray("pathList");
  settings->endArray();
  if (settings->contains("path") && len == 0) {
    qDebug() << "Using old client path storage";
    m_clientPathList += settings->value("path", "proxmark3").toString();
  } else {
    int arrayLen = settings->beginReadArray("pathList");
    for (int i = 0; i < arrayLen; i++) {
      settings->setArrayIndex(i);
      QString path = settings->value("path").toString();
      if (!path.isEmpty())
        m_clientPathList += path;
    }
    settings->endArray();
  }
  settings->endGroup();

  ui->PM3_pathBox->clear();
  for (const QString &clientPath : qAsConst(m_clientPathList))
    ui->PM3_pathBox->addItem(clientPath);
}

void MainWindow::saveClientPathList() {
  settings->beginGroup("Client_Path");
  if (settings->contains("path")) {
    qDebug() << "Upgrading client path storage";
    QString oldPath = settings->value("path").toString();
    if (!oldPath.isEmpty() && !m_clientPathList.contains(oldPath))
      m_clientPathList.append(oldPath);
    settings->remove("path");
  }

  settings->beginWriteArray("pathList");
  for (int i = 0; i < m_clientPathList.size(); i++) {
    settings->setArrayIndex(i);
    settings->setValue("path", m_clientPathList[i]);
  }
  settings->endArray();
  settings->endGroup();
}
// ***********************************************

void MainWindow::on_MF_Attack_darksideButton_clicked() {
  setState(false);
  mifare->darkside();
  setState(true);
}

void MainWindow::on_Set_Client_startArgsEdit_editingFinished() {
  settings->beginGroup("Client_Args");
  settings->setValue("args", ui->Set_Client_startArgsEdit->text());
  settings->endGroup();
}

void MainWindow::on_Set_Client_forceEnabledBox_stateChanged(int arg1) {
  settings->beginGroup("Client_forceButtonsEnabled");
  keepButtonsEnabled = (arg1 == Qt::Checked);
  settings->setValue("state", keepButtonsEnabled);
  settings->endGroup();
  if (keepButtonsEnabled)
    setButtonsEnabled(true);
}

void MainWindow::on_Set_UI_setLanguageButton_clicked() {
  Util::chooseLanguage(settings, this);
}

void MainWindow::on_PM3_refreshPortButton_clicked() {
  on_portSearchTimer_timeout();
}

void MainWindow::on_Set_Client_envScriptEdit_editingFinished() {
  settings->beginGroup("Client_Env");
  settings->setValue("scriptPath", ui->Set_Client_envScriptEdit->text());
  settings->endGroup();
}

void MainWindow::on_Set_Client_workingDirEdit_editingFinished() {
  settings->beginGroup("Client_Env");
  settings->setValue("workingDir", ui->Set_Client_workingDirEdit->text());
  settings->endGroup();
}

void MainWindow::on_Set_Client_configPathEdit_editingFinished() {
  settings->beginGroup("Client_Env");
  settings->setValue("extConfigFilePath",
                     ui->Set_Client_configPathEdit->text());
  settings->endGroup();
}

void MainWindow::on_Set_Client_keepClientActiveBox_stateChanged(int arg1) {
  settings->beginGroup("Client_keepClientActive");
  keepClientActive = (arg1 == Qt::Checked);
  settings->setValue("state", keepClientActive);
  settings->endGroup();
  emit setSerialListener(!keepClientActive);
}

void MainWindow::on_LF_LFConf_freqSlider_valueChanged(int value) {
  onLFfreqConfChanged(value, true);
}

void MainWindow::onLFfreqConfChanged(int divisor, bool isCustomized) {
  ui->LF_LFConf_freqDivisorBox->blockSignals(true);
  ui->LF_LFConf_freqSlider->blockSignals(true);

  if (isCustomized)
    ui->LF_LFConf_freqOtherButton->setChecked(true);
  else if (divisor == 95)
    ui->LF_LFConf_freq125kButton->setChecked(true);
  else if (divisor == 88)
    ui->LF_LFConf_freq134kButton->setChecked(true);
  ui->LF_LFConf_freqLabel->setText(
      tr("Actural Freq: ") +
      QString("%1kHz").arg(LF::divisor2Freq(divisor), 0, 'f', 3));
  ui->LF_LFConf_freqDivisorBox->setValue(divisor);
  ui->LF_LFConf_freqSlider->setValue(divisor);

  ui->LF_LFConf_freqDivisorBox->blockSignals(false);
  ui->LF_LFConf_freqSlider->blockSignals(false);
}

void MainWindow::on_LF_LFConf_freqDivisorBox_valueChanged(int arg1) {
  onLFfreqConfChanged(arg1, true);
}

void MainWindow::on_LF_LFConf_freq125kButton_clicked() {
  onLFfreqConfChanged(95, false);
}

void MainWindow::on_LF_LFConf_freq134kButton_clicked() {
  onLFfreqConfChanged(88, false);
}

void MainWindow::on_LF_Op_searchButton_clicked() {
  setState(false);
  lf->search();
  setState(true);
}

void MainWindow::on_LF_Op_readButton_clicked() {
  setState(false);
  lf->read();
  setState(true);
}

void MainWindow::on_LF_Op_tuneButton_clicked() {
  setState(false);
  lf->tune();
  setState(true);
}

void MainWindow::on_LF_Op_sniffButton_clicked() {
  setState(false);
  lf->sniff();
  setState(true);
}

void MainWindow::dockInit() {
  setDockNestingEnabled(true);
  QDockWidget *dock;
  QWidget *widget;
  int count = ui->funcTab->count();
  qDebug() << "dock count" << count;
  for (int i = 0; i < count; i++) {
    dock = new QDockWidget(ui->funcTab->tabText(0), this);
    qDebug() << "dock name" << ui->funcTab->tabText(0);
    dock->setFeatures(
        QDockWidget::DockWidgetFloatable |
        QDockWidget::DockWidgetMovable); // movable is necessary, otherwise the
                                         // dock cannot be dragged
    dock->setAllowedAreas(Qt::BottomDockWidgetArea);
    dock->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    widget = ui->funcTab->widget(0);
    dock->setWidget(widget);
    if (widget->objectName() == "rawTab")
      Util::setRawTab(dock, i);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
    if (!dockList.isEmpty())
      tabifyDockWidget(dockList[0], dock);
    dockList.append(dock);
  }
  ui->funcTab->setVisible(false);
  dockList[0]->setVisible(true);
  dockList[0]->raise();
}

void MainWindow::contextMenuEvent(QContextMenuEvent *event) {
  contextMenu->exec(event->globalPos());
}

void MainWindow::on_LF_LFConf_getButton_clicked() {
  setState(false);
  lf->getLFConfig();
  setState(true);
}

void MainWindow::on_LF_LFConf_setButton_clicked() {
  LF::LFConfig config;
  setState(false);
  config.divisor = ui->LF_LFConf_freqDivisorBox->value();
  config.bitsPerSample = ui->LF_LFConf_bitsPerSampleBox->value();
  config.decimation = ui->LF_LFConf_decimationBox->value();
  config.averaging = ui->LF_LFConf_averagingBox->isChecked();
  config.triggerThreshold = ui->LF_LFConf_thresholdBox->value();
  config.samplesToSkip = ui->LF_LFConf_skipsBox->value();
  lf->setLFConfig(config);
  Util::gotoRawTab();
  setState(true);
}

void MainWindow::on_LF_LFConf_resetButton_clicked() {
  setState(false);
  lf->resetLFConfig();
  setState(true);
}

void MainWindow::on_Set_Client_configFileBox_currentIndexChanged(int index) {
  ui->Set_Client_configPathEdit->setVisible(
      ui->Set_Client_configFileBox->itemData(index).toString() == "(ext)");
  settings->beginGroup("Client_Env");
  settings->setValue("configFile", ui->Set_Client_configFileBox->currentData());
  settings->endGroup();
}

void MainWindow::on_Set_UI_Opacity_Box_valueChanged(int arg1) {
  ui->Set_UI_Opacity_slider->blockSignals(true);
  ui->Set_UI_Opacity_slider->setValue(arg1);
  setWindowOpacity(arg1 / 100.0);
  settings->beginGroup("UI");
  settings->setValue("Opacity", ui->Set_UI_Opacity_Box->value());
  settings->endGroup();
  ui->Set_UI_Opacity_slider->blockSignals(false);
}

void MainWindow::on_Set_UI_Theme_setButton_clicked() {
  settings->beginGroup("UI");
  settings->setValue("Theme_Name",
                     ui->Set_UI_Theme_nameBox->currentData().toString());
  settings->endGroup();
}

void MainWindow::on_Set_UI_Font_setButton_clicked() {
  QFont font = ui->Set_UI_Font_nameBox->currentFont();
  font.setPointSize(ui->Set_UI_Font_sizeBox->value());
  QApplication::setFont(font, "QWidget");

  settings->beginGroup("UI");
  settings->setValue("Font_Name",
                     ui->Set_UI_Font_nameBox->currentFont().family());
  settings->setValue("Font_Size", ui->Set_UI_Font_sizeBox->value());
  settings->endGroup();
}

void MainWindow::on_Set_UI_DataFont_setButton_clicked() {
  QFont font = ui->Set_UI_DataFont_nameBox->currentFont();
  font.setPointSize(ui->Set_UI_DataFont_sizeBox->value());
  ui->MF_dataWidget->setFont(font);
  ui->MF_keyWidget->setFont(font);

  settings->beginGroup("UI");
  settings->setValue("DataFont_Name",
                     ui->Set_UI_DataFont_nameBox->currentFont().family());
  settings->setValue("DataFont_Size", ui->Set_UI_DataFont_sizeBox->value());
  settings->endGroup();
}

void MainWindow::on_Set_UI_CMDFont_setButton_clicked() {
  QFont font = ui->Set_UI_CMDFont_nameBox->currentFont();
  font.setPointSize(ui->Set_UI_CMDFont_sizeBox->value());
  ui->Raw_outputEdit->setFont(font);

  settings->beginGroup("UI");
  settings->setValue("CMDFont_Name",
                     ui->Set_UI_CMDFont_nameBox->currentFont().family());
  settings->setValue("CMDFont_Size", ui->Set_UI_CMDFont_sizeBox->value());
  settings->endGroup();
}

// 确保函数名 on_MF_Attack_autopwnButton_clicked
// 这里的 MF_Attack_autopwnButton 必须是你 UI 里的按钮 ID
void MainWindow::on_MF_Attack_autopwnButton_clicked() {
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("一键破解 (Autopwn) 提示"));
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setTextFormat(Qt::RichText);
    msgBox.setText(tr("即将开始全自动一键破解。<br><br>"
                      "⚠️ <b>重要提醒 (防变砖)：</b><br>"
                      "受底层程序的特性限制，一键破解自动导出的 Dump 文件中有时会将隐藏的 <b>KeyB 强行填充为全 0</b>！<br><br>"
                      "👉 如果您破解完毕后准备【克隆写卡】，<b>强烈建议</b>不要直接使用它自动保存的文件，而是：<br>"
                      "1. 等待破解结束，密码自动同步到右侧面板。<br>"
                      "2. 点击下方读取面板的【读取选中块】读出卡片的完整真实数据。<br>"
                      "3. 点击下方的【导出数据】来保存最终安全的写卡文件！<br><br>"
                      "是否确认开始破解？"));
    QPushButton *continueBtn = msgBox.addButton(tr("我已知晓，开始破解"), QMessageBox::AcceptRole);
    msgBox.addButton(tr("取消 (Cancel)"), QMessageBox::RejectRole);
    msgBox.setDefaultButton(continueBtn);

    msgBox.exec();
    if (msgBox.clickedButton() != continueBtn) return; // 拦截：点取消就终止

    setState(false);
    mifare->autopwn();
    setState(true);
}

void MainWindow::on_MF_Attack_rf08sButton_clicked() {
  setState(false);
  mifare->scriptRf08s(); // 确保 mifare.h 里已经声明了 scriptrf08s()
  setState(true);
}


void MainWindow::on_MF_RW_generateEmptyDataButton_clicked() {
    // --- 🚨 核心防砖拦截：必须先有真实的第 0 块 ---
    QString block0Text = ui->MF_dataWidget->item(0, 2) ? ui->MF_dataWidget->item(0, 2)->text().remove(" ").toUpper() : "";
    if (block0Text.length() != 32 || block0Text == "00000000000000000000000000000000") {
        QMessageBox::critical(this, tr("危险拦截 (防变砖)"),
                              tr("未检测到真实的卡片数据！\n\n"
                                 "直接写入空的第 0 块会导致魔术卡永久损坏（变砖）。\n"
                                 "👉 解决办法：请先将卡片放在读卡器上，点击面板上的【Read (读取)】或至少读取【0 扇区】，然后再生成空数据。"));
        return; // 强行终止，绝不往下走
    }

    int blocks = mifare->cardType.block_size;

    for (int i = 0; i < blocks; i++) {
        if (i == 0) continue; // 绝对保留真实的0块
        bool isTrailer = (i < 128 && ((i + 1) % 4 == 0)) || ((i + 1) % 16 == 0);
        if (isTrailer) mifare->data_setData(i, "FFFFFFFFFFFFFF078069FFFFFFFFFFFF");
        else mifare->data_setData(i, "00000000000000000000000000000000");
    }

    mifare->data_syncWithDataWidget(true, 0);

    ui->MF_dataWidget->blockSignals(true);
    ui->MF_selectAllBox->blockSignals(true);
    ui->MF_dataWidget->item(0, 1)->setCheckState(Qt::Unchecked);
    for (int i = 1; i < blocks; i++) {
        ui->MF_dataWidget->item(i, 1)->setCheckState(Qt::Checked);
    }
    ui->MF_selectAllBox->setCheckState(Qt::PartiallyChecked);
    ui->MF_dataWidget->blockSignals(false);
    ui->MF_selectAllBox->blockSignals(false);

    QString emptyDumpPath = QDir::homePath() + "/empty-dump.bin";
    mifare->data_saveDataFile(emptyDumpPath, true);

    QMessageBox::information(this, tr("空数据生成完毕"),
                             tr("面板数据已转换为初始白卡状态！\n\n"
                                "✅ 已自动在您的【用户目录】生成 <b>empty-dump.bin</b>\n"
                                "👉 您现在可以点击【高级清卡 (Wipe)】按钮进行深度清理。"));
}

// ==========================================
// ✨ 高级清卡 (Wipe Card) 核心逻辑
// ==========================================
void MainWindow::on_MF_RW_wipeCardButton_clicked() {
    // --- 🚨 核心防砖拦截：必须先有真实的第 0 块 ---
    QString block0Full = ui->MF_dataWidget->item(0, 2) ? ui->MF_dataWidget->item(0, 2)->text().remove(" ").toUpper() : "";
    if (block0Full.length() != 32 || block0Full == "00000000000000000000000000000000") {
        QMessageBox::critical(this, tr("危险拦截 (防变砖)"),
                              tr("当前缺少真实的第 0 块（卡号与厂商信息）！\n\n"
                                 "强制清卡前必须知道原卡的真实卡号，否则恢复数据会导致卡片报废。\n"
                                 "👉 解决办法：请先读取卡片信息获取真实的第 0 块数据。"));
        return; // 强行终止
    }

    QString emptyDumpPath = QDir::homePath() + "/empty-dump.bin";

    // 1. 在后台静默生成安全的空 Dump（绝对使用真实的 block0Full）
    QByteArray emptyData;
    int blocks = mifare->cardType.block_size;

    for (int i = 0; i < blocks; i++) {
        QString hexStr;
        if (i == 0) hexStr = block0Full; // 使用刚才强制校验过的真实数据
        else {
            bool isTrailer = (i < 128 && ((i + 1) % 4 == 0)) || ((i + 1) % 16 == 0);
            hexStr = isTrailer ? "FFFFFFFFFFFFFF078069FFFFFFFFFFFF" : "00000000000000000000000000000000";
        }
        for(int k = 0; k < 32; k += 2) {
            emptyData.append(static_cast<char>(hexStr.mid(k, 2).toUShort(nullptr, 16)));
        }
    }

    // 每次点击高级清卡，都重新覆盖生成一次 empty-dump.bin，确保它包含的是【当前这张卡】的真实 UID
    QFile file(emptyDumpPath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(emptyData);
        file.close();
    }

    // 2. 绘制高颜值对话框 (逻辑保持不变)
    QDialog dialog(this);
    dialog.setWindowTitle(tr("高级清卡参数设置"));
    dialog.resize(500, 200);
    QFormLayout form(&dialog);

    form.addRow(new QLabel(tr("<span style='color: #E53935; font-size: 14px;'><b>【 ⚠️ 警告：此操作将使用原卡密码覆盖整卡数据】</b></span>")));
    QLabel *targetHint = new QLabel(tr("将执行指令: restore -f empty-dump.bin -k [原卡密钥] --ka\n此操作将把加密卡彻底重置为空白卡状态（已自动安全保留第0块）。"));
    targetHint->setStyleSheet("color: #666666; font-size: 12px; margin-bottom: 10px;");
    targetHint->setAlignment(Qt::AlignCenter);
    form.addRow(targetHint);

    form.addRow(new QLabel(tr("<span style='color: #1976D2;'><b>【 📄 1. 空白数据文件 (Dump)】</b></span>")));
    QHBoxLayout *dumpLayout = new QHBoxLayout();
    QLineEdit *dumpEdit = new QLineEdit(&dialog);
    dumpEdit->setText(emptyDumpPath);
    dumpEdit->setMinimumWidth(250);
    QPushButton *dumpBtn = new QPushButton(tr("浏览"), &dialog);
    dumpLayout->addWidget(dumpEdit);
    dumpLayout->addWidget(dumpBtn);
    form.addRow(tr("Dump 文件:"), dumpLayout);

    connect(dumpBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getOpenFileName(this, tr("选择空数据文件"), QDir::homePath(), "Dump Files(*.bin *.dump);;All Files(*.*)");
        if (!path.isEmpty()) dumpEdit->setText(path);
    });

    form.addRow(new QLabel(" "));

    form.addRow(new QLabel(tr("<span style='color: #43A047;'><b>【 🔑 2. 原卡密钥文件 (Key)】</b></span>")));
    QHBoxLayout *keyLayout = new QHBoxLayout();
    QLineEdit *keyEdit = new QLineEdit(&dialog);

    QString uid = mifare->data_getUID();
    QString autoKeyPath = "";
    if (!uid.isEmpty() && uid != "00000000" && uid != "FFFFFFFF") {
        QDir userDir(QDir::homePath());
        QStringList keyFilters;
        keyFilters << "*" + uid.toLower() + "*key*.bin" << "*" + uid.toUpper() + "*key*.bin";
        QStringList keyFiles = userDir.entryList(keyFilters, QDir::Files, QDir::Time);
        if (!keyFiles.isEmpty()) autoKeyPath = userDir.absoluteFilePath(keyFiles.first());
    }
    keyEdit->setText(autoKeyPath);
    keyEdit->setMinimumWidth(250);

    QPushButton *keyBtn = new QPushButton(tr("浏览"), &dialog);
    keyLayout->addWidget(keyEdit);
    keyLayout->addWidget(keyBtn);
    form.addRow(tr("Key 文件:"), keyLayout);

    connect(keyBtn, &QPushButton::clicked, [&]() {
        QString path = QFileDialog::getOpenFileName(this, tr("选择原卡密钥"), QDir::homePath(), "Key Files(*.bin *.dump *.key);;All Files(*.*)");
        if (!path.isEmpty()) keyEdit->setText(path);
    });

    form.addRow(new QLabel(" "));

    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);
    connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        QString finalDump = dumpEdit->text();
        QString finalKey = keyEdit->text();

        if (finalDump.isEmpty() || finalKey.isEmpty()) {
            QMessageBox::critical(this, tr("错误"), tr("请提供完整的 Dump 文件和 Key 文件路径！"));
            return;
        }
        if (!QFile::exists(finalDump) || !QFile::exists(finalKey)) {
            QMessageBox::critical(this, tr("错误"), tr("文件不存在，请检查路径是否正确！"));
            return;
        }

        mifare->restore(finalDump, finalKey, false, false);
    }
}

void MainWindow::on_MF_File_compareButton_clicked(){
    QString title = tr("请选择要进行校验的 Dump 文件:");
    QString filename = QFileDialog::getOpenFileName(
        this, title, "./",
        tr("Binary Data Files(*.bin *.dump)") + ";;" +
            tr("Text Data Files(*.txt *.eml)") + ";;" + tr("All Files(*.*)"));

    if (filename != "") {
        if (!mifare->data_compareDataFile(filename)) {
            QMessageBox::information(this, tr("错误"), tr("无法打开文件:\n") + filename);
        }
    }
}


void MainWindow::on_MF_RW_modifyCardCodeButton_clicked(){
    // === 🚨 防护 1：检查是否选择了 0 块 ===
    if (ui->MF_RW_blockBox->currentText() != "0") {
        QMessageBox::warning(this, tr("操作提示"),
                             tr("修改卡号 (UID) 必须在 0 块进行！\n👉 请在左侧下拉框中将 Block 设置为 0。"));
        return;
    }

    // === 🚨 防护 2：检查是否已经读取了数据 ===
    QString currentData = ui->MF_RW_dataEdit->text().remove(" ").toUpper();
    if (currentData.length() != 32 || currentData == "00000000000000000000000000000000") {
        QMessageBox::warning(this, tr("操作提示"),
                             tr("当前 0 块数据为空或无效！\n👉 请先将卡片放在读卡器上，点击【读取单个块】读取 0 块数据。"));
        return;
    }

    // === 🚨 防护 3：防砖校验 (提取原卡数据并验证 BCC) ===
    QString origUid = currentData.left(8);
    QString origBcc = currentData.mid(8, 2);
    QString origTail = currentData.right(22); // SAK(2) + ATQA(4) + 厂商信息(16)

    quint8 calcOrigBcc = 0;
    for(int i = 0; i < 4; i++) {
        calcOrigBcc ^= origUid.mid(i * 2, 2).toUInt(nullptr, 16);
    }
    QString calcOrigBccStr = QString::number(calcOrigBcc, 16).rightJustified(2, '0').toUpper();

    if (calcOrigBccStr != origBcc) {
        QMessageBox::critical(this, tr("危险拦截 (防变砖)"),
                              tr("当前数据的 BCC 校验和不匹配！\n\n可能原因：\n1. 这是一张 7 字节长 UID 卡\n2. 数据读取不完整\n\n为防止强行写入导致卡片报废，已自动拦截修改操作！"));
        return;
    }

    // === ✨ 构建高颜值弹窗 ===
    QDialog dialog(this);
    dialog.setWindowTitle(tr("智能卡号 (UID) 修改器"));
    dialog.setMinimumWidth(550); // 加宽以容纳漂亮的拆解视图

    QFormLayout form(&dialog);
    form.setSizeConstraint(QLayout::SetFixedSize); // 高度随内容自动适应

    // ======== 第一部分：原卡信息 ========
    form.addRow(new QLabel(tr("<span style='color: #666666;'><b>【第一部分：当前原卡信息】</b></span>")));

    QLineEdit *origUidEdit = new QLineEdit(&dialog);
    origUidEdit->setText(origUid);
    origUidEdit->setReadOnly(true);
    origUidEdit->setStyleSheet("background-color: #f0f0f0; color: #666666;");
    origUidEdit->setAlignment(Qt::AlignCenter);
    form.addRow(tr("原卡 UID:"), origUidEdit);

    form.addRow(new QLabel(" ")); // 空行分隔

    // ======== 第二部分：修改设置 ========
    form.addRow(new QLabel(tr("<span style='color: #1976D2;'><b>【第二部分：设置新卡号】</b></span>")));

    QLabel *hintLabel = new QLabel(tr("输入 8 位 16 进制字符，程序将自动计算 BCC 校验码。"));
    hintLabel->setStyleSheet("color: #888888; font-size: 12px;");
    form.addRow(hintLabel);

    QLineEdit *newUidEdit = new QLineEdit(&dialog);
    newUidEdit->setText(origUid);
    newUidEdit->setMaxLength(8);
    newUidEdit->setAlignment(Qt::AlignCenter);

    QRegularExpression rx("^[0-9a-fA-F]{8}$");
    newUidEdit->setValidator(new QRegularExpressionValidator(rx, this));

    QLabel *bccLabel = new QLabel(&dialog);
    bccLabel->setStyleSheet("color: #E53935; font-weight: bold;");

    QHBoxLayout *uidLayout = new QHBoxLayout();
    uidLayout->addWidget(newUidEdit);
    uidLayout->addWidget(new QLabel("  自动 BCC: "));
    uidLayout->addWidget(bccLabel);
    form.addRow(tr("新 UID:"), uidLayout);

    form.addRow(new QLabel(" ")); // 空白间距

    // ======== 第三部分：终极高颜值数据拆解视图 ========
    QWidget *previewWidget = new QWidget(&dialog);
    QHBoxLayout *previewLayout = new QHBoxLayout(previewWidget);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(8); // 各区块间距

    // 格式化辅助函数：给 Hex 加空格
    auto formatHex = [](const QString& hexStr) {
        QString res;
        for(int i = 0; i < hexStr.length(); i += 2) res += hexStr.mid(i, 2) + " ";
        return res.trimmed();
    };

    // UI 构建辅助函数：创建单个拆解区块
    auto createBlock = [](const QString& title, QLabel*& dataLabel) -> QVBoxLayout* {
        QVBoxLayout *vbox = new QVBoxLayout();
        vbox->setSpacing(2);

        dataLabel = new QLabel();
        dataLabel->setStyleSheet("font-family: Consolas, monospace; font-size: 14px; color: #43A047;");
        dataLabel->setAlignment(Qt::AlignCenter);

        QFrame *line = new QFrame(); // 红线
        line->setFrameShape(QFrame::HLine);
        line->setStyleSheet("color: #E53935; background-color: #E53935; max-height: 2px;");

        QLabel *titleLabel = new QLabel(title);
        titleLabel->setStyleSheet("color: #E53935; font-size: 11px; font-weight: bold;");
        titleLabel->setAlignment(Qt::AlignCenter);

        vbox->addWidget(dataLabel);
        vbox->addWidget(line);
        vbox->addWidget(titleLabel);
        return vbox;
    };

    // 声明负责显示的 Label 指针
    QLabel *uidDataLabel, *bccDataLabel, *ataDataLabel, *manuDataLabel;

    previewLayout->addLayout(createBlock("UID", uidDataLabel));
    previewLayout->addLayout(createBlock("BCC", bccDataLabel));
    previewLayout->addLayout(createBlock("SAK/ATQA", ataDataLabel));
    previewLayout->addLayout(createBlock("厂商数据", manuDataLabel));

    // 初始化固定不变的数据尾巴 (SAK+ATQA = 6位，厂商数据 = 16位)
    ataDataLabel->setText(formatHex(origTail.left(6)));
    manuDataLabel->setText(formatHex(origTail.right(16)));

    form.addRow(tr("数据预览:"), previewWidget);

    // 用来存放最终给写卡用的无空格的 32 位文本
    QString finalDataToSave;

    // === 动态计算与联动 ===
    auto updatePreview = [&]() {
        QString inputUid = newUidEdit->text().toUpper();
        if (inputUid.length() == 8) {
            quint8 newBcc = 0;
            for(int i = 0; i < 4; i++) {
                newBcc ^= inputUid.mid(i * 2, 2).toUInt(nullptr, 16);
            }
            QString newBccStr = QString::number(newBcc, 16).rightJustified(2, '0').toUpper();

            // 联动更新文本
            bccLabel->setText(newBccStr);
            uidDataLabel->setText(formatHex(inputUid));
            bccDataLabel->setText(newBccStr);

            // 记录好无空格的最终版准备回填
            finalDataToSave = inputUid + newBccStr + origTail;
        } else {
            bccLabel->setText("等待...");
            uidDataLabel->setText("-");
            bccDataLabel->setText("-");
        }
    };

    QObject::connect(newUidEdit, &QLineEdit::textChanged, updatePreview);
    updatePreview(); // 初始刷一次

    form.addRow(new QLabel(" "));

    // ======== 确认按钮 ========
    QDialogButtonBox buttonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    form.addRow(&buttonBox);
    QObject::connect(&buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // === 弹窗处理结果 ===
    if (dialog.exec() == QDialog::Accepted) {
        if (newUidEdit->text().length() != 8) {
            QMessageBox::warning(this, tr("错误"), tr("UID 长度必须正好是 8 个字符！"));
            return;
        }

        // 写入到界面的 Data 输入框中 (无空格版，防止UI越界)
        ui->MF_RW_dataEdit->setText(finalDataToSave);

        QMessageBox::information(this, tr("准备就绪"),
                                 tr("新数据已填入 Data 框中。\n\n"
                                    "👉 点击下方的【写入单个块】按钮即可写入卡片！"));
    }

}

