// https://github.com/KubaO/stackoverflown/tree/master/questions/eventloop-hang-25038829
#include <QtWidgets>
#include <QtConcurrent>
#include <random>

std::default_random_engine reng;

int ilog2(qint64 val) {
  Q_ASSERT(val >= 0);
  int ret = -1;
  while (val != 0) { val >>= 1; ret++; }
  return ret;
}

/// The value binned to contain at most \a binaryDigits significant digits.
/// The less significant digits are reset to zero.
qint64 binned(qint64 value, int binaryDigits)
{
  Q_ASSERT(binaryDigits > 0);
  qint64 mask = -1;
  int clrBits = ilog2(value) - binaryDigits;
  if (clrBits > 0) mask <<= clrBits;
  return value & mask;
}

/// A safely destructible thread for perusal by QObjects.
class Thread final : public QThread {
  Q_OBJECT
  void run() override {
    connect(QAbstractEventDispatcher::instance(this),
            &QAbstractEventDispatcher::aboutToBlock,
            this, &Thread::aboutToBlock);
    QThread::run();
  }
  QAtomicInt inDestructor;
public:
  using QThread::QThread;
  /// Take an object and prevent timer resource leaks when the object is about
  /// to become threadless.
  void takeObject(QObject *obj) {
    // Work around to prevent
    // QBasicTimer::stop: Failed. Possibly trying to stop from a different thread
    static constexpr char kRegistered[] = "__ThreadRegistered";
    static constexpr char kMoved[] = "__Moved";
    if (!obj->property(kRegistered).isValid()) {
      QObject::connect(this, &Thread::finished, obj, [this, obj]{
        if (!inDestructor.load() || obj->thread() != this)
          return;
        // The object is about to become threadless
        Q_ASSERT(obj->thread() == QThread::currentThread());
        obj->setProperty(kMoved, true);
        obj->moveToThread(this->thread());
      }, Qt::DirectConnection);
      QObject::connect(this, &QObject::destroyed, obj, [obj]{
        if (!obj->thread()) {
          obj->moveToThread(QThread::currentThread());
          obj->setProperty(kRegistered, {});
        }
        else if (obj->thread() == QThread::currentThread() && obj->property(kMoved).isValid()) {
          obj->setProperty(kMoved, {});
          QCoreApplication::sendPostedEvents(obj, QEvent::MetaCall);
        }
        else if (obj->thread()->eventDispatcher())
          QTimer::singleShot(0, obj, [obj]{ obj->setProperty(kRegistered, {}); });
      }, Qt::DirectConnection);

      obj->setProperty(kRegistered, true);
    }
    obj->moveToThread(this);
  }
  ~Thread() override {
    inDestructor.store(1);
    requestInterruption();
    quit();
    wait();
  }
  Q_SIGNAL void aboutToBlock();
};

/// An application that monitors event loops in all threads.
class MonitoringApp : public QApplication {
  Q_OBJECT
  Q_PROPERTY(int timeout READ timeout WRITE setTimeout MEMBER m_timeout)
  Q_PROPERTY(int updatePeriod READ updatePeriod WRITE setUpdatePeriod MEMBER m_updatePeriod)
public:
  using Histogram = QMap<qint64, uint>;
  using Base = QApplication;
private:
  struct ThreadData {
    /// A saturating, binned histogram of event handling durations for given thread.
    Histogram histogram;
    /// Number of milliseconds between the epoch and when the event handler on this thread
    /// was entered, or zero if no event handler is running.
    qint64 ping = 0;
    /// Number of milliseconds between the epoch and when the last histogram update for
    /// this thread was broadcast
    qint64 update = 0;
    /// Whether the thread's event loop is considered stuck at the moment
    bool stuck = false;
    /// Whether the thread is newly detected
    bool newThread = true;
  };
  using Threads = QMap<QThread*, ThreadData>;
  QMutex m_mutex;
  Threads m_threads;
  int m_timeout = 1000;
  int m_updatePeriod = 250;

  class StuckEventLoopNotifier : public QObject {
    MonitoringApp *m_app;
    QBasicTimer m_timer;
    struct State { QThread *thread; qint64 elapsed; };
    QVector<State> m_toEmit;
    void timerEvent(QTimerEvent * ev) override {
      if (ev->timerId() != m_timer.timerId()) return;
      int timeout = m_app->m_timeout;
      auto now = QDateTime::currentMSecsSinceEpoch();
      m_toEmit.clear();
      QMutexLocker lock(&m_app->m_mutex);
      for (auto it = m_app->m_threads.begin(); it != m_app->m_threads.end(); ++it) {
        if (it->ping == 0) continue;
        qint64 elapsed = now - it->ping;
        it->stuck = elapsed > timeout;
        m_toEmit.push_back({it.key(), it->stuck ? elapsed : 0});
      }
      lock.unlock();
      for (auto &sig : qAsConst(m_toEmit)) emit m_app->loopStateChanged(sig.thread, sig.elapsed);
    }
  public:
    explicit StuckEventLoopNotifier(MonitoringApp * app) : m_app(app) {
      m_timer.start(100, Qt::CoarseTimer, this);
    }
  };
  StuckEventLoopNotifier m_notifier{this};
  Thread m_notifierThread;
  void threadFinishedSlot() {
    auto const thread = qobject_cast<QThread*>(QObject::sender());
    QMutexLocker lock(&m_mutex);
    auto it = m_threads.find(thread);
    if (it == m_threads.end()) return;
    auto const histogram(it->histogram);
    bool stuck = it->stuck;
    m_threads.erase(it);
    lock.unlock();
    emit newHistogram(thread, histogram);
    if (stuck) emit loopStateChanged(thread, 0);
    emit threadFinished(thread);
  }
  Q_SIGNAL void newThreadSignal(QThread*, const QString &);
protected:
  bool notify(QObject * receiver, QEvent * event) override {
    auto const curThread = QThread::currentThread();
    QElapsedTimer timer;
    auto now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker lock(&m_mutex);
    auto &thread = m_threads[curThread];
    thread.ping = now;
    bool newThread = false;
    std::swap(newThread, thread.newThread);
    lock.unlock();
    if (newThread) {
      connect(curThread, &QThread::finished, this, &MonitoringApp::threadFinishedSlot);
      struct Event : QEvent {
        QThread *thread;
        QPointer<MonitoringApp> app;
        explicit Event(QThread *thread, MonitoringApp *app) :
          QEvent(QEvent::None), thread(thread), app(app) {}
        ~Event() override {
          // objectName() can only be invoked from the object's thread
          emit app->newThreadSignal(thread, thread->objectName());
        }
      };
      QCoreApplication::postEvent(curThread, new Event(curThread, this));
    }
    timer.start();
    auto result = Base::notify(receiver, event); // This is where the event loop can get "stuck".
    auto duration = binned(timer.elapsed(), 3);
    now += duration;
    lock.relock();
    if (thread.histogram[duration] < std::numeric_limits<Histogram::mapped_type>::max())
      ++thread.histogram[duration];
    thread.ping = 0;
    qint64 sinceLastUpdate = now - thread.update;
    if (sinceLastUpdate >= m_updatePeriod) {
      auto const histogram = thread.histogram;
      thread.update = now;
      lock.unlock();
      emit newHistogram(curThread, histogram);
    }
    return result;
  }
public:
  explicit MonitoringApp(int & argc, char ** argv);
  /// The event loop for a given thread has gotten stuck, or unstuck.
  /** A zero elapsed time indicates that the loop is not stuck. The signal will be
    * emitted periodically with increasing values of `elapsed` for a given thread as long
    * as the loop is stuck. The thread might not exist when this notification is received. */
  Q_SIGNAL void loopStateChanged(QThread *, int elapsed);
  /// The first event was received in a newly started thread's event loop.
  /** The thread might not exist when this notification is received. */
  Q_SIGNAL void newThread(QThread *, const QString & threadName);
  /// The thread has a new histogram available.
  /** This signal is not sent more often than each updatePeriod().
    * The thread might not exist when this notification is received. */
  Q_SIGNAL void newHistogram(QThread *, const MonitoringApp::Histogram &);
  /// The thread has finished.
  /** The thread might not exist when this notification is received. A newHistogram
    * signal is always emitted prior to this signal's emission. */
  Q_SIGNAL void threadFinished(QThread *);
  /// The maximum number of milliseconds an event handler can run before the event loop
  /// is considered stuck.
  int timeout() const { return m_timeout; }
  Q_SLOT void setTimeout(int timeout) { m_timeout = timeout; }
  int updatePeriod() const { return m_updatePeriod; }
  Q_SLOT void setUpdatePeriod(int updatePeriod) { m_updatePeriod = updatePeriod; }
};
Q_DECLARE_METATYPE(MonitoringApp::Histogram)

MonitoringApp::MonitoringApp(int &argc, char **argv) :
  MonitoringApp::Base(argc, argv)
{
  qRegisterMetaType<MonitoringApp::Histogram>();
  connect(this, &MonitoringApp::newThreadSignal, this, &MonitoringApp::newThread,
          Qt::QueuedConnection);
  m_notifierThread.setObjectName("notifierThread");
  m_notifierThread.takeObject(&m_notifier);
  m_notifierThread.start();
}

QImage renderHistogram(const MonitoringApp::Histogram &h) {
  const int blockX = 2, blockY = 2;
  QImage img(1 + h.size() * blockX, 32 * blockY, QImage::Format_ARGB32_Premultiplied);
  img.fill(Qt::white);
  QPainter p(&img);
  int x = 0;
  for (auto it = h.begin(); it != h.end(); ++it) {
    qreal key = it.key() > 0 ? log2(it.key()) : 0.0;
    QBrush b = QColor::fromHsv(qRound(240.0*(1.0 - key/32.0)), 255, 255);
    p.fillRect(QRectF(x, img.height(), blockX, -log2(it.value()) * blockY), b);
    x += blockX;
  }
  return img;
}

class MonitoringViewModel : public QStandardItemModel {
  Q_OBJECT
  struct Item {
    bool set = false;
    QStandardItem *caption = 0, *histogram = 0;
    void setCaption(QThread* thread, const QString &name) {
      auto text = QStringLiteral("0x%1 \"%2\"").arg(std::intptr_t(thread), 0, 16).arg(name);
      caption->setText(text);
    }
  };
  QMap<QThread*, Item> m_threadItems;
  Item &itemFor(QThread *thread, bool set = true) {
    Item &item = m_threadItems[thread];
    if (set && !item.set) {
      item.caption = new QStandardItem;
      item.histogram = new QStandardItem;
      item.caption->setEditable(false);
      item.histogram->setEditable(false);
      int row = rowCount() ? 1 : 0;
      insertRow(row);
      setItem(row, 0, item.caption);
      setItem(row, 1, item.histogram);
      item.set = true;
      newHistogram(thread, MonitoringApp::Histogram());
    }
    return item;
  }
  void newThread(QThread *thread, const QString &name) {
    itemFor(thread).setCaption(thread, name);
  }
  void newHistogramImage(QThread *thread, const QImage &img) {
    auto &item = itemFor(thread, false);
    if (!item.set) return;
    item.histogram->setSizeHint(img.size());
    item.histogram->setData(img, Qt::DecorationRole);
  }
  Q_SIGNAL void newHistogramImageSignal(QThread *thread, const QImage &img);
  void newHistogram(QThread *thread, const MonitoringApp::Histogram &histogram) {
    QtConcurrent::run([this, thread, histogram]{
      emit newHistogramImageSignal(thread, renderHistogram(histogram));
    });
  }
  void loopStateChanged(QThread *thread, int elapsed) {
    auto &item = itemFor(thread);
    item.caption->setData(elapsed ? QColor(Qt::red) : QColor(Qt::transparent), Qt::BackgroundColorRole);
  }
  void threadFinished(QThread *thread) {
    auto &item = itemFor(thread);
    item.caption->setText(QStringLiteral("Finished %1").arg(item.caption->text()));
    item.set = false;
  }
public:
  MonitoringViewModel(QObject *parent = 0) : QStandardItemModel(parent) {
    connect(this, &MonitoringViewModel::newHistogramImageSignal,
            this, &MonitoringViewModel::newHistogramImage);
    auto app = qobject_cast<MonitoringApp*>(qApp);
    connect(app, &MonitoringApp::newThread, this, &MonitoringViewModel::newThread);
    connect(app, &MonitoringApp::newHistogram,  this, &MonitoringViewModel::newHistogram);
    connect(app, &MonitoringApp::threadFinished, this, &MonitoringViewModel::threadFinished);
    connect(app, &MonitoringApp::loopStateChanged, this, &MonitoringViewModel::loopStateChanged);
  }
};

class WorkerObject : public QObject {
  Q_OBJECT
  int m_trials = 2000;
  double m_probability = 0.2;
  QBasicTimer m_timer;
  void timerEvent(QTimerEvent * ev) override {
    if (ev->timerId() != m_timer.timerId()) return;
    QThread::msleep(std::binomial_distribution<>(m_trials, m_probability)(reng));
  }
public:
  using QObject::QObject;
  Q_SIGNAL void stopped();
  Q_SLOT void start() { m_timer.start(0, this); }
  Q_SLOT void stop() { m_timer.stop(); emit stopped(); }
  int trials() const { return m_trials; }
  void setTrials(int trials) { m_trials = trials; }
  double probability() const { return m_probability; }
  void setProbability(double p) { m_probability = p; }
};

int main(int argc, char *argv[])
{
  MonitoringApp app(argc, argv);
  MonitoringViewModel model;
  WorkerObject workerObject;
  Thread workerThread;
  workerThread.setObjectName("workerThread");

  QWidget w;
  QGridLayout layout(&w);
  QTableView view;
  QLabel timeoutLabel;
  QSlider timeout(Qt::Horizontal);
  QGroupBox worker("Worker Thread");
  worker.setCheckable(true);
  worker.setChecked(false);
  QGridLayout wLayout(&worker);
  QLabel rangeLabel, probabilityLabel;
  QSlider range(Qt::Horizontal), probability(Qt::Horizontal);

  timeoutLabel.setMinimumWidth(50);
  QObject::connect(&timeout, &QSlider::valueChanged, &timeoutLabel, (void(QLabel::*)(int))&QLabel::setNum);
  timeout.setMinimum(50);
  timeout.setMaximum(5000);
  timeout.setValue(app.timeout());
  view.setModel(&model);
  view.verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

  layout.addWidget(&view, 0, 0, 1, 3);
  layout.addWidget(new QLabel("Timeout"), 1, 0);
  layout.addWidget(&timeoutLabel, 1, 1);
  layout.addWidget(&timeout, 1, 2);
  layout.addWidget(&worker, 2, 0, 1, 3);

  QObject::connect(&range, &QAbstractSlider::valueChanged, [&](int p){
    rangeLabel.setText(QString("Range %1 ms").arg(p));
    workerObject.setTrials(p);
  });
  QObject::connect(&probability, &QAbstractSlider::valueChanged, [&](int p){
    double prob = p / (double)probability.maximum();
    probabilityLabel.setText(QString("Probability %1").arg(prob, 0, 'g', 2));
    workerObject.setProbability(prob);
  });
  range.setMaximum(10000);
  range.setValue(workerObject.trials());
  probability.setValue(workerObject.probability() * probability.maximum());

  wLayout.addWidget(new QLabel("Sleep Time Binomial Distribution"), 0, 0, 1, 2);
  wLayout.addWidget(&rangeLabel, 1, 0);
  wLayout.addWidget(&range, 2, 0);
  wLayout.addWidget(&probabilityLabel, 1, 1);
  wLayout.addWidget(&probability, 2, 1);

  QObject::connect(&workerObject, &WorkerObject::stopped, &workerThread, &Thread::quit);
  QObject::connect(&worker, &QGroupBox::toggled, [&](bool run) {
    if (run) {
      workerThread.start();
      QMetaObject::invokeMethod(&workerObject, "start");
    } else
      QMetaObject::invokeMethod(&workerObject, "stop");
  });
  QObject::connect(&timeout, &QAbstractSlider::valueChanged, &app, &MonitoringApp::setTimeout);
  workerThread.takeObject(&workerObject);
  w.show();
  return app.exec();
}

#include "main.moc"
