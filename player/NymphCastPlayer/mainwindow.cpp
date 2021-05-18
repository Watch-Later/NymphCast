#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <iostream>
#include <vector>

#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QCoreApplication>
#include <QTime>
#include <QFile>
#include <QTextStream>
#include <QSettings>
#include <QStringList>
#include <QStandardPaths>
#include <QDir>

#if defined(Q_OS_ANDROID)
#include <QtAndroidExtras>
#include <QAndroidJniEnvironment>
#include <QtAndroid>

class MediaItem : public QObject {
	Q_OBJECT
	Q_PROPERTY(QString title READ title CONSTANT)
	Q_PROPERTY(QString album READ album CONSTANT)
	Q_PROPERTY(QString artist READ artist CONSTANT)
	Q_PROPERTY(QString path READ path CONSTANT)
	
	QString m_title;
	QString m_album;
	QString m_artist;
	QString m_path;
	
public:
	MediaItem(const QString title, const QString album, const QString artist, const QString path, 
																		QObject *parent = nullptr) 
		: QObject(parent), m_title(title), m_album(album), m_artist(artist), m_path(path) { }
	
	QString title() const { return m_title; }
	QString album() const { return m_album; }
	QString artist() const { return m_artist; }
	QString path() const { return m_path; }
};

static int pfd[2];
static pthread_t thr;
static const char* tag = "NymphCastPlayer";

#include <android/log.h>

static void* thread_func(void*) {
    ssize_t rdsz;
    char buf[128];
    while ((rdsz = read(pfd[0], buf, sizeof buf - 1)) > 0) {
        //if (buf[rdsz - 1] == '\n') --rdsz; // Remove newline if it exists.
        buf[rdsz] = 0;  // add null-terminator
        __android_log_write(ANDROID_LOG_DEBUG, tag, buf);
    }
    
    return 0;
}


int start_logger(const char* app_name) {
    tag = app_name;

    /* make stdout line-buffered and stderr unbuffered */
    setvbuf(stdout, 0, _IOLBF, 0);
    setvbuf(stderr, 0, _IONBF, 0);

    /* create the pipe and redirect stdout and stderr */
    pipe(pfd);
    dup2(pfd[1], 1);
    dup2(pfd[1], 2);

    /* spawn the logging thread */
    if (pthread_create(&thr, 0, thread_func, 0) == -1) { return -1; }    
    pthread_detach(thr);
    return 0;
}

#endif


// Static declarations
uint32_t MainWindow::serverHandle;
NymphCastClient MainWindow::client;

	
Q_DECLARE_METATYPE(NymphPlaybackStatus);


MainWindow::MainWindow(QWidget *parent) :	 QMainWindow(parent), ui(new Ui::MainWindow) {
	ui->setupUi(this);
	
	// Register custom types.
	qRegisterMetaType<NymphPlaybackStatus>("NymphPlaybackStatus");
	qRegisterMetaType<uint32_t>("uint32_t");
	
	// Set application options.
	QCoreApplication::setApplicationName("NymphCast Player");
	QCoreApplication::setApplicationVersion("v0.1-alpha");
	QCoreApplication::setOrganizationName("Nyanko");
	
	// Set configured or default stylesheet. Read out current value.
	// Skip stylesheet if file isn't found.
	QSettings settings;
	if (!settings.contains("stylesheet")) {
		settings.setValue("stylesheet", "default.css");
	}
	
	QString sFile = settings.value("stylesheet", "default.css").toString();
	QFile file(sFile);
	if (file.exists()) {
		file.open(QIODevice::ReadOnly);
		QString ssheet = QString::fromLocal8Bit(file.readAll());
		setStyleSheet(ssheet);
	}
	else {
		std::cerr << "Stylesheet file " << sFile.toStdString() << " not found." << std::endl;
	}
	
	// Set up UI connections.
	// Menu
	connect(ui->actionConnect, SIGNAL(triggered()), this, SLOT(connectServer()));
	connect(ui->actionDisconnect, SIGNAL(triggered()), this, SLOT(disconnectServer()));
	connect(ui->actionQuit, SIGNAL(triggered()), this, SLOT(quit()));
	connect(ui->actionAbout, SIGNAL(triggered()), this, SLOT(about()));
	connect(ui->actionFile, SIGNAL(triggered()), this, SLOT(castFile()));
	connect(ui->actionURL, SIGNAL(triggered()), this, SLOT(castUrl()));
	
	// Tabs
    // Player tab.
	connect(ui->addButton, SIGNAL(clicked()), this, SLOT(addFile()));
	connect(ui->removeButton, SIGNAL(clicked()), this, SLOT(removeFile()));
    connect(ui->beginToolButton, SIGNAL(clicked()), this, SLOT(rewind()));
	connect(ui->endToolButton, SIGNAL(clicked()), this, SLOT(forward()));
	connect(ui->playToolButton, SIGNAL(clicked()), this, SLOT(play()));
	connect(ui->stopToolButton, SIGNAL(clicked()), this, SLOT(stop()));
	connect(ui->pauseToolButton, SIGNAL(clicked()), this, SLOT(pause()));
    connect(ui->soundToolButton, SIGNAL(clicked()), this, SLOT(mute()));
	connect(ui->volumeSlider, SIGNAL(sliderReleased()), this, SLOT(adjustVolume()));
	connect(ui->positionSlider, SIGNAL(sliderReleased()), this, SLOT(seek()));
    
    // Remotes tab.
	connect(ui->refreshRemotesToolButton, SIGNAL(clicked()), this, SLOT(remoteListRefresh()));
	connect(ui->connectToolButton, SIGNAL(clicked()), this, SLOT(remoteConnectSelected()));
	connect(ui->disconnectToolButton, SIGNAL(clicked()), this, SLOT(remoteDisconnectSelected()));
	
	// Apps tab.
	connect(ui->updateRemoteAppsButton, SIGNAL(clicked()), this, SLOT(appsListRefresh()));
	connect(ui->remoteAppLineEdit, SIGNAL(returnPressed()), this, SLOT(sendCommand()));
    
    // Apps (GUI) tab.
    connect(ui->appTabGuiHomeButton, SIGNAL(clicked()), this, SLOT(appsHome()));
    //connect(ui->appTabGuiTextBrowser, SIGNAL(anchorClicked(QUrl)), this, SLOT(anchorClicked(QUrl)));
    connect(ui->appTabGuiTextBrowser, SIGNAL(linkClicked(QUrl)), this, SLOT(anchorClicked(QUrl)));
    
    // Shares tab.
    connect(ui->sharesScanButton, SIGNAL(clicked()), this, SLOT(scanForShares()));
    connect(ui->sharesPlayButton, SIGNAL(clicked()), this, SLOT(playSelectedShare()));
    
    using namespace std::placeholders; 
    ui->appTabGuiTextBrowser->setResourceHandler(std::bind(&MainWindow::loadResource, this, _1));
	
	connect(this, SIGNAL(playbackStatusChange(uint32_t, NymphPlaybackStatus)), 
			this, SLOT(setPlaying(uint32_t, NymphPlaybackStatus)));
	
	// NymphCast client SDK callbacks.
	using namespace std::placeholders;
	client.setStatusUpdateCallback(std::bind(&MainWindow::statusUpdateCallback,	this, _1, _2));
    
    // Set values.
    ui->sharesTreeView->setModel(&sharesModel);
	
#if defined(Q_OS_ANDROID)
    // We need to redirect stdout/stderr. This requires starting a new thread here.
    start_logger(tag);
    
	// On Android platforms we read in the media files into the playlist as they are in standard
	// locations. This is also a work-around for QTBUG-83372: 
	// https://bugreports.qt.io/browse/QTBUG-83372
	
	// First, disable the 'add' and 'remove' buttons as these won't be used on Android.
	ui->addButton->setEnabled(false);
    ui->addButton->setVisible(false);
	ui->removeButton->setEnabled(false);
    ui->removeButton->setVisible(false);
	
	if(!QAndroidJniObject::isClassAvailable("com/nyanko/nymphcastplayer/NymphCast")) {
		qDebug() << "Java class is missing.";
		return;
	}
	
	// Next, read the local media files and add them to the list, sorting music and videos.
	//QStringList audio = QStandardPaths::locateAll(QStandardPaths::MusicLocation, QString());
	//QStringList video = QStandardPaths::locateAll(QStandardPaths::MoviesLocation, QString());
	QAndroidJniObject audioObj = QAndroidJniObject::callStaticObjectMethod(
                            "com/nyanko/nymphcastplayer/NymphCast",
							"loadAudio",
							"(Landroid/content/Context;)Ljava/util/ArrayList;",
							QtAndroid::androidContext().object());
		
	for (int i = 0; i < audioObj.callMethod<jint>("size"); ++i) {
		// Add item to the list.
		QAndroidJniObject track = audioObj.callObjectMethod("get", "(I)Ljava/lang/Object;", i);
		const QString title = track.callObjectMethod("getTitle", "()Ljava/lang/String;").toString();
		const QString album = track.callObjectMethod("getAlbum", "()Ljava/lang/String;").toString();
		const QString artist = track.callObjectMethod("getArtist", "()Ljava/lang/String;").toString();
		const QString path = track.callObjectMethod("getPath", "()Ljava/lang/String;").toString();
		QListWidgetItem *newItem = new QListWidgetItem;
		newItem->setText(artist + " - " + title);
		newItem->setData(Qt::UserRole, QVariant(path));
		ui->mediaListWidget->addItem(newItem);
		
		// Debug
		std::cout << path.toStdString() << std::endl;
	}
	
	/* for (int i = 0; i < video.size(); ++i) {
		// Add item to the list.
		QListWidgetItem *newItem = new QListWidgetItem;
		newItem->setText(video[i]);
		newItem->setData(Qt::UserRole, QVariant(video[i]));
		ui->mediaListWidget->addItem(newItem);
	}	 */
#else
	// Reload stored file paths, if any.
	QFile playlist;
	playlist.setFileName("filepaths.conf");
	playlist.open(QIODevice::ReadOnly);
	QTextStream textStream(&playlist);
	QString line;
	while (!(line = textStream.readLine()).isNull()) {
		QFileInfo finf(line);
		QListWidgetItem *newItem = new QListWidgetItem;
		newItem->setText(finf.fileName());
		newItem->setData(Qt::UserRole, QVariant(line));
		ui->mediaListWidget->addItem(newItem);
	}
	
	playlist.close();
#endif
}

MainWindow::~MainWindow() {
	delete ui;
}


// --- STATUS UPDATE CALLBACK ---
void MainWindow::statusUpdateCallback(uint32_t handle, NymphPlaybackStatus status) {
	// Send the data along to the slot on the GUI thread.
	emit playbackStatusChange(handle, status);
}


// --- SET PLAYING ---
void MainWindow::setPlaying(uint32_t /*handle*/, NymphPlaybackStatus status) {
	if (status.playing) {
        std::cout << "Status: Set playing..." << std::endl;
		// Remote player is active. Read out 'status.status' to get the full status.
		ui->playToolButton->setEnabled(false);
		ui->stopToolButton->setEnabled(true);
		
		// Set position & duration.
		QTime position(0, 0);
		position = position.addSecs((int64_t) status.position);
		QTime duration(0, 0);
		duration = duration.addSecs(status.duration);
		ui->durationLabel->setText(position.toString("hh:mm:ss") + " / " + 
														duration.toString("hh:mm:ss"));
														
		ui->positionSlider->setValue((status.position / status.duration) * 100);
		
		ui->volumeSlider->setValue(status.volume);
	}
	else {
        std::cout << "Status: Set not playing..." << std::endl;
		// Remote player is not active.
		ui->playToolButton->setEnabled(true);
		ui->stopToolButton->setEnabled(false);
		
		ui->durationLabel->setText("0:00 / 0:00");
		ui->positionSlider->setValue(0);
		
		ui->volumeSlider->setValue(status.volume);
		
		if (playingTrack) {
            playingTrack = false;
            std::cout << "Status: Playing track, check autoplay..." << std::endl;
			// We finished playing the currently selected track.
			// If auto-play is on, play the next track.
            if (ui->singlePlayCheckBox->isChecked() == false) {
                std::cout << "Status: Move to next track..." << std::endl;
                // Next track.
                int crow = ui->mediaListWidget->currentRow();
                if (++crow == ui->mediaListWidget->count()) {
                    // Restart at top.
                    crow = 0;
                }
                
                std::cout << "Status: Start playing track index " << crow << "..." << std::endl;
                
                ui->mediaListWidget->setCurrentRow(crow);
                play();
            }
		}
	}
}


void MainWindow::connectServer() {
	if (connected) { return; }
	
	// Ask for the IP address of the server.
	QString ip = QInputDialog::getText(this, tr("NymphCast Receiver"), tr("Please provide the NymphCast receiver IP address."));
	if (ip.isEmpty()) { return; }	
	
	// Connect to localhost NymphRPC server, standard port.
	if (!client.connectServer(ip.toStdString(), 0, serverHandle)) {
		QMessageBox::warning(this, tr("Failed to connect"), tr("The selected server could not be connected to."));
		return;
	}
	
	// Update server name label.
	ui->remoteLabel->setText("Connected to " + ip);
	
	// Successful connect.
	connected = true;
}


// --- CONNECT SERVER IP ---
void MainWindow::connectServerIP(std::string ip) {
	if (connected) { return; }
	
	// Connect to localhost NymphRPC server, standard port.
	if (!client.connectServer(ip, 0, serverHandle)) {
		QMessageBox::warning(this, tr("Failed to connect"), tr("The selected server could not be connected to."));
		return;
	}
	
	// TODO: update server name label.
	ui->remoteLabel->setText("Connected to " + QString::fromStdString(ip));
	
	// Successful connect.
	connected = true;
}


// --- DISCONNECT SERVER ---
void MainWindow::disconnectServer() {
	if (!connected) { return; }
	
	client.disconnectServer(serverHandle);
	
	ui->remoteLabel->setText("Disconnected.");
	
	connected = false;
}


// --- REMOTE LIST REFRESH ---
// Refresh the list of remote servers on the network.
void MainWindow::remoteListRefresh() {
	// Get the current list.
	remotes = client.findServers();
	
	// Update the list with any changed items.
	// Target the 'remotesListWidget' widget.
	ui->remotesListWidget->clear(); // FIXME: just resetting the whole thing for now.
	for (uint32_t i = 0; i < remotes.size(); ++i) {
		//new QListWidgetItem(remotes[i].ipv4 + " (" + remotes[i].name + ")", ui->remotesListWidget);
		QListWidgetItem *newItem = new QListWidgetItem;
		newItem->setText(QString::fromStdString(remotes[i].ipv4 + " (" + remotes[i].name + ")"));
		//newItem->setData(Qt::UserRole, QVariant(QString::fromStdString(remotes[i].ipv4)));
		newItem->setData(Qt::UserRole, QVariant(i));
		ui->remotesListWidget->insertItem(i, newItem);
	}
	
}


// --- REMOTE CONNECT SELECTED ---
// Connect to the selected remote server.
void MainWindow::remoteConnectSelected() {
	if (connected) { return; }
	
	//QListWidgetItem* item = ui->remotesListWidget->currentItem();
	//QString ip = item->data(Qt::UserRole).toString();
	QList<QListWidgetItem*> items = ui->remotesListWidget->selectedItems();
	
	if (items.size() == 0) { 
		QMessageBox::warning(this, tr("No selection"), tr("No remotes were selected."));
		return; 
	}
	
	// The first (index 0) remote is connected to as the master remote. Any further remotes are
	// sent to the master remote as slave remotes.
	
	// Connect to the server.
	//connectServerIP(ip.toStdString());
	int ref = items[0]->data(Qt::UserRole).toInt();
	connectServerIP(remotes[ref].ipv4);
	
	if (!connected || items.size() < 2) { return; }
	
	std::vector<NymphCastRemote> slaves;
	for (int i = 1; i < items.size(); ++i) {
        ref = items[i]->data(Qt::UserRole).toInt();
		slaves.push_back(remotes[ref]);
	}
	
	client.addSlaves(serverHandle, slaves);
}


// --- REMOTE DISCONNECT SELECTED ---
void MainWindow::remoteDisconnectSelected() {
	// FIXME: Redirect to the plain disconnectServer() function for now.
	// With the multi-server functionality implemented, this should disconnect the selected remote.
	disconnectServer();
}


// --- CAST FILE ---
void MainWindow::castFile() {
	if (!connected) { return; }
	
	// Open file.
	QString filename = QFileDialog::getOpenFileName(this, tr("Open media file"));
	if (filename.isEmpty()) { return; }
	
	if (client.castFile(serverHandle, filename.toStdString())) {
		// Playing back file now. Update status.
		playingTrack = true;
	}
	else {
		// TODO: Display error.
	}
}


// --- CAST URL ---
void MainWindow::castUrl() {
	if (!connected) { return; }
	
	// Open file.
	QString url = QInputDialog::getText(this, tr("Cast URL"), tr("Copy in the URL to cast."));
	if (url.isEmpty()) { return; }
	
	client.castUrl(serverHandle, url.toStdString());
}


// --- ADD FILE ---
void MainWindow::addFile() {
	// Select file from filesystem, add to playlist.
	// Use stored directory location, if any.
	QSettings settings;
	QString dir = settings.value("openFileDir").toString();
	QString filename = QFileDialog::getOpenFileName(this, tr("Open media file"), dir);
	if (filename.isEmpty()) { return; }
	
	// Update current folder.
	settings.setValue("openFileDir", filename);
	
	// Check file.
	QFileInfo finf(filename);
	if (!finf.isFile()) { 
		QMessageBox::warning(this, tr("Failed to open file"), tr("The selected file could not be opened."));
		return;
	}
	
	// Add it.
	QListWidgetItem *newItem = new QListWidgetItem;
	newItem->setText(finf.fileName());
	newItem->setData(Qt::UserRole, QVariant(filename));
	ui->mediaListWidget->addItem(newItem);
	
	// Store path of added file to reload on restart. Append to file.
	QFile playlist;
	playlist.setFileName("filepaths.conf");
	playlist.open(QIODevice::WriteOnly | QIODevice::Append);
	QTextStream textStream(&playlist);
	textStream << filename << "\n";
	playlist.close();
}


// --- REMOVE FILE ---
void MainWindow::removeFile() {
	// Remove currently selected filename(s) from the playlist.
	QListWidgetItem* item = ui->mediaListWidget->currentItem();
	ui->mediaListWidget->removeItemWidget(item);
	delete item;
	
	// TODO: Remove stored file path.
	// FIXME: Clear and rewrite the file contents for now.
	QFile playlist;
	playlist.setFileName("filepaths.conf");
	playlist.open(QIODevice::WriteOnly | QIODevice::Truncate);
	int size = ui->mediaListWidget->count();
	QTextStream textStream(&playlist);
	for (int i = 0; i < size; ++i) {
		QListWidgetItem* item = ui->mediaListWidget->item(i);
		QString filename = item->data(Qt::UserRole).toString();
		textStream << filename << "\n";
	}
	
	playlist.close();
}


// --- PLAY ---
void MainWindow::play() {
	if (!connected) { return; }
	
	// Start playing the currently selected track if it isn't already playing. 
	// Else pause or unpause playback.
	if (playingTrack) {
		client.playbackStart(serverHandle);
	}
	else {
		QListWidgetItem* item = ui->mediaListWidget->currentItem();
		if (item == 0) { 
			QMessageBox::warning(this, tr("No file selected"), tr("Please first select a file to play."));
			return; 
		}
		
		QString filename = item->data(Qt::UserRole).toString();
		
        if (client.castFile(serverHandle, filename.toStdString())) {
            // Playing back file now. Update status.
           playingTrack = true;
        }
	}
	
}


// --- STOP ---
void MainWindow::stop() {
	//
	if (!connected) { return; }
    
    playingTrack = false;
	client.playbackStop(serverHandle);
}


// --- PAUSE ---
void MainWindow::pause() {
	//
	if (!connected) { return; }
	
	client.playbackPause(serverHandle);
}


// --- FORWARD ---
void MainWindow::forward() {
	//
	if (!connected) { return; }
	
	client.playbackForward(serverHandle);
}


// --- REWIND ---
void MainWindow::rewind() {
	//
	if (!connected) { return; }
	
	client.playbackRewind(serverHandle);
}


// --- SEEK ---
void MainWindow::seek() {
	if (!connected) { return; }
	
	// Read out location on seek bar.
	uint8_t location = ui->positionSlider->value();
	
	client.playbackSeek(serverHandle, location);
}


// --- MUTE ---
void MainWindow::mute() {
	//
	if (!connected) { return; }
	
	if (!muted) {
		client.volumeSet(serverHandle, 0);
		muted = true;
	}
	else {
		client.volumeSet(serverHandle, ui->volumeSlider->value());
		muted = false;
	}
}


// --- ADJUST VOLUME ---
void MainWindow::adjustVolume() {
	if (!connected) { return; }
	int value = ui->volumeSlider->value();
	if (value < 0 || value > 128) { return; }
	
	client.volumeSet(serverHandle, value);
}


// --- APPS LIST REFRESH ---
void MainWindow::appsListRefresh() {
	if (!connected) { return; }
	
	// Get list of apps from the remote server.
	std::string appList = client.getApplicationList(serverHandle);
	
	// Update local list.
	ui->remoteAppsComboBox->clear();
	QStringList appItems = (QString::fromStdString(appList)).split("\n", QString::SkipEmptyParts);
	ui->remoteAppsComboBox->addItems(appItems);
}


// --- SEND COMMAND ---
void MainWindow::sendCommand() {
	if (!connected) { return; }
	
	// Read the data in the line edit and send it to the remote app.
	// Get the appID from the currently selected item in the app list combobox.
	QString currentItem = ui->remoteAppsComboBox->currentText();
	
	std::string appId = currentItem.toStdString();
	std::string message = ui->remoteAppLineEdit->text().toStdString();
	
	// Append the command to the output field.
	ui->remoteAppTextEdit->appendPlainText(ui->remoteAppLineEdit->text());
	ui->remoteAppTextEdit->appendPlainText("\n");
	
	// Clear the input field.
	ui->remoteAppLineEdit->clear();
	
	std::string response = client.sendApplicationMessage(serverHandle, appId, message);
	
	// Append the response to the output field.
	ui->remoteAppTextEdit->appendPlainText(QString::fromStdString(response));
	ui->remoteAppTextEdit->appendPlainText("\n");
}


// --- APPS HOME ---
void MainWindow::appsHome() {
    if (!connected) { return; }
    
    // Request the starting Apps page from the remote.
    QString page = QString::fromStdString(client.loadResource(serverHandle, std::string(), 
                                                                               "apps.html"));
    
    
    // Set the received HTML into the target widget.
    ui->appTabGuiTextBrowser->setHtml(page);
}


// --- ANCHOR CLICKED ---
void MainWindow::anchorClicked(const QUrl &link) {
	// Debug
	std::cout << "anchorClicked: " << link.path().toStdString() << std::endl;
	
    // Parse URL string for the command desired.
    QStringList list = link.path().split("/", QString::SkipEmptyParts);
    
    // Process command.
    if (list.size() < 1) { return; }
    
    if (list[0] == "start") {
        // Start an app here, which should be listed in the second slot.
        if (list.size() < 2) { return; }
        
        // Try to load the index page for the specified app.
        QString page = QString::fromStdString(client.loadResource(serverHandle, 
                                                                  list[1].toStdString(), 
                                                                   "index.html"));
        
        if (page.isEmpty()) { 
            QMessageBox::warning(this, tr("Failed to start"), tr("The selected app could not be started."));
            return; 
        }
        
        // Set the received HTML into the target widget.
        ui->appTabGuiTextBrowser->setHtml(page);
    }
    else {
        // Assume the first entry contains an app name, followed by commands.
        // TODO: validate app names here.
		if (list.size() < 2) { return; }
		std::string response = client.sendApplicationMessage(serverHandle, list[0].toStdString(), 
																			list[1].toStdString());
		// TODO: use response.
    }
}


// --- LOAD RESOURCE ---
QByteArray MainWindow::loadResource(const QUrl &name) {
    // Parse the URL for the desired resource.
    QFileInfo dir(name.path());
    QString qAppId = dir.path();
    std::string filename = name.fileName().toStdString();
    
    // FIXME: Hack to deal with weird QLiteHtml behaviour with relative URLs.
    if (qAppId.startsWith("/")) {
        qAppId.remove(0, 1);
    }
    
    std::string appId = qAppId.toStdString();
    
    QByteArray page = QByteArray::fromStdString(client.loadResource(serverHandle, appId, filename));
    return page;
}


// --- SCAN FOR SHARES ---
void MainWindow::scanForShares() {
    // Scan for media server instances on the network.
    std::vector<NymphCastRemote> mediaservers = client.findShares();
    if (mediaservers.empty()) {
        QMessageBox::warning(this, tr("No media servers found."), tr("No media servers found."));
        return;
    }
    
    // For each media server, request the shared file list.
    sharesModel.clear();
    mediaFiles.clear();
    QStandardItem* parentItem = sharesModel.invisibleRootItem();
    for (uint32_t i = 0; i < mediaservers.size(); ++i) {
        std::vector<NymphMediaFile> files = client.getShares(mediaservers[i]);
        if (files.empty()) { continue; }
        
        // Insert into model. Use the media server's host name as top folder, with the shared
        // files inserted underneath it.
        QStandardItem* item = new QStandardItem(QString::fromStdString(mediaservers[i].name));
        item->setSelectable(false);
        for (uint32_t j = 0; j < files.size(); ++j) {
            QStandardItem* fn = new QStandardItem(QString::fromStdString(files[j].name));
            QList<QVariant> ids;
			ids.append(QVariant(i));
			ids.append(QVariant(j));
            ids.append(QVariant(files[j].id));
            mediaFiles.push_back(files);
            ids.append(QVariant((uint32_t) mediaFiles.size()));
            
            fn->setData(QVariant(ids), Qt::UserRole);
            item->appendRow(fn);
        }
        
        parentItem->appendRow(item);
    }
}


// --- PLAY SELECTED SHARE --
void MainWindow::playSelectedShare() {
	// FIXME: use selected remote(s) as check & input here.
    //if (!connected) { return; }
    
    // Get the currently selected file name and obtain the ID.
    QModelIndexList indexes = ui->sharesTreeView->selectionModel()->selectedIndexes();
    if (indexes.size() == 0) {
        QMessageBox::warning(this, tr("No file selected."), tr("No media file selected."));
        return;
    }
    else if (indexes.size() > 1) {
        QMessageBox::warning(this, tr("No file selected."), tr("No media file selected."));
        return;
    }
    
    QMap<int, QVariant> data = sharesModel.itemData(indexes[0]);
    QList<QVariant> ids = data[Qt::UserRole].toList();
    
    // Obtain list of target receivers.
    QList<QListWidgetItem*> items = ui->remotesListWidget->selectedItems();
    std::vector<NymphCastRemote> receivers;
	for (int i = 0; i < items.size(); ++i) {
        int ref = items[i]->data(Qt::UserRole).toInt();
		receivers.push_back(remotes[ref]);
	}
    
    // Play file via media server.
    if (!client.playShare(mediaFiles[ids[0].toInt()][ids[1].toInt()], receivers)) {
         //
    }
}


// --- ABOUT ---
void MainWindow::about() {
	QMessageBox::about(this, tr("About NymphCast Player."), tr("NymphCast Player is a simple demonstration player for the NymphCast client SDK."));
}


// --- QUIT ---
void MainWindow::quit() {
	exit(0);
}
