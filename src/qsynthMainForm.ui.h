// qsynthMainForm.ui.h
//
// ui.h extension file, included from the uic-generated form implementation.
/****************************************************************************
   Copyright (C) 2003-2004, rncbc aka Rui Nuno Capela. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

*****************************************************************************/

#include <qapplication.h>
#include <qeventloop.h>
#include <qmessagebox.h>
#include <qdragobject.h>
#include <qregexp.h>
#include <qtimer.h>
#include <qpopupmenu.h>

#include <math.h>

#include "config.h"

#include "qsynthAbout.h"

// Timer constant stuff.
#define QSYNTH_TIMER_MSECS  200
#define QSYNTH_DELAY_MSECS  200

// Scale factors.
#define QSYNTH_GAIN_SCALE   10.0
#define QSYNTH_REVERB_SCALE 100.0
#define QSYNTH_CHORUS_SCALE 10.0

// Notification pipe descriptors
#define QSYNTH_FDNIL     -1
#define QSYNTH_FDREAD     0
#define QSYNTH_FDWRITE    1

static int g_fdStdout[2] = { QSYNTH_FDNIL, QSYNTH_FDNIL };

static qsynthEngine *g_pCurrentEngine = NULL;

//-------------------------------------------------------------------------
// Needed for server mode.
static fluid_cmd_handler_t* qsynth_newclient ( void* data, char* )
{
    return ::new_fluid_cmd_handler((fluid_synth_t*) data);
}

//-------------------------------------------------------------------------
// Audio driver processing stub.

int qsynth_process ( void *pvData, int len, int nin, float **in, int nout, float **out )
{
    qsynthEngine *pEngine = (qsynthEngine *) pvData;
	// Call the synthesizer process function to fill
	// the output buffers with its audio output.
    if (::fluid_synth_process(pEngine->pSynth, len, nin, in, nout, out) != 0)
		return -1;
	// Now find the peak level for this buffer run...
	if (pEngine == g_pCurrentEngine) {
		for (int i = 0; i < nout; i++) {
			float *out_i = out[i];
			for (int j = 0; j < len; j++) {
				float fValue = out_i[j];
				if (pEngine->fMeterValue[i & 1] < fValue)
					pEngine->fMeterValue[i & 1] = fValue;
			}
		}
	}
	// Surely a success :)
	return 0;
}
 
//-------------------------------------------------------------------------
// Midi router stubs to have some midi activity feedback.

#define QSYNTH_MIDI_NOTE_OFF            0x80
#define QSYNTH_MIDI_NOTE_ON             0x90
#define QSYNTH_MIDI_CONTROL_CHANGE      0xb0
#define QSYNTH_MIDI_PROGRAM_CHANGE      0xc0

#define QSYNTH_MIDI_CC_BANK_SELECT_MSB  0x00
#define QSYNTH_MIDI_CC_BANK_SELECT_LSB  0x20
#define QSYNTH_MIDI_CC_ALL_SOUND_OFF    0x78

struct qsynth_midi_channel
{
    int iEvent;     // Event occurrence accumulator.
    int iState;     // Activity state tracker.
    int iChange;    // Change activity accumulator.
};

static int                  g_iMidiChannels  = 0;
static qsynth_midi_channel *g_pMidiChannels  = NULL;

static void qsynth_midi_event ( qsynthEngine *pEngine, fluid_midi_event_t *pMidiEvent )
{
    pEngine->iMidiEvent++;
    
    if (g_pMidiChannels && pEngine == g_pCurrentEngine) {
        int iChan = ::fluid_midi_event_get_channel(pMidiEvent);
#ifdef CONFIG_DEBUG
        int iType = ::fluid_midi_event_get_type(pMidiEvent);
        int iKey  = ::fluid_midi_event_get_control(pMidiEvent);
        int iVal  = ::fluid_midi_event_get_value(pMidiEvent);
        fprintf(stderr, "Type=%03d (0x%02x) Chan=%02d Key=%03d (0x%02x) Val=%03d (0x%02x).\n",
            iType, iType, iChan, iKey, iKey, iVal, iVal);
#endif
        if (iChan >= 0 && iChan < g_iMidiChannels) {
            int iCC;
            switch (::fluid_midi_event_get_type(pMidiEvent)) {
              case QSYNTH_MIDI_CONTROL_CHANGE:
                // Avoid bank selects or global control changes...
                iCC = ::fluid_midi_event_get_control(pMidiEvent);
                if (iCC == QSYNTH_MIDI_CC_BANK_SELECT_MSB ||
                    iCC == QSYNTH_MIDI_CC_BANK_SELECT_LSB ||
                    iCC >= QSYNTH_MIDI_CC_ALL_SOUND_OFF)
                    break;
                // Fall thru...
              case QSYNTH_MIDI_PROGRAM_CHANGE:
                g_pMidiChannels[iChan].iChange++;
                // Fall thru, again...
              case QSYNTH_MIDI_NOTE_ON:
              case QSYNTH_MIDI_NOTE_OFF:
                g_pMidiChannels[iChan].iEvent++;
                break;
            }
        }
    }
}

static int qsynth_dump_postrouter (void *pvData, fluid_midi_event_t *pMidiEvent)
{
    qsynthEngine *pEngine = (qsynthEngine *) pvData;
    qsynth_midi_event(pEngine, pMidiEvent);
    return ::fluid_midi_dump_postrouter(pEngine->pSynth, pMidiEvent);
}

static int qsynth_handle_midi_event (void *pvData, fluid_midi_event_t *pMidiEvent)
{
    qsynthEngine *pEngine = (qsynthEngine *) pvData;
    qsynth_midi_event(pEngine, pMidiEvent);
    return ::fluid_synth_handle_midi_event(pEngine->pSynth, pMidiEvent);
}


//-------------------------------------------------------------------------
// Scaling & Clipping helpers.

static int qsynth_iscale_clip ( double fScale, double fValue )
{
#ifdef CONFIG_ROUND
    int iValue = (int) ::round(fScale * fValue);
#else
    double fIPart = 0.0;
    double fFPart = ::modf(fScale * fValue, &fIPart);
    int iValue = (int) fIPart;
    if (fFPart >= +0.5)
        iValue++;
    else
    if (fFPart <= -0.5)
        iValue--;
#endif

    if (iValue < 0)
        iValue = 0;
    else if (iValue > 100)
        iValue = 100;
        
    return iValue;
}

static double qsynth_fscale_clip ( int iValue, double fScale )
{
    double fValue = ((double) iValue / fScale);

    const double fMax = (100.0 / fScale);
    if (fValue < 0.0)
        fValue = 0.0;
    else if (fValue > fMax)
        fValue = fMax;
        
    return fValue;
}


//-------------------------------------------------------------------------
// qsynthMainForm -- Main window form implementation.

// Kind of constructor.
void qsynthMainForm::init (void)
{
    m_pOptions = NULL;

    m_iTimerDelay = 0;

    m_iCurrentTab = -1;

    m_pStdoutNotifier = NULL;

    m_iGainChanged   = 0;
    m_iReverbChanged = 0;
    m_iChorusChanged = 0;

    m_iGainUpdated   = 0;
    m_iReverbUpdated = 0;
    m_iChorusUpdated = 0;

    // All forms are to be created later on setup.
    m_pMessagesForm  = NULL;
    m_pChannelsForm  = NULL;
    
    // TabBar management.
    QObject::connect(TabBar, SIGNAL(selected(int)), this, SLOT(tabSelect(int)));
    QObject::connect(TabBar, SIGNAL(contextMenuRequested(qsynthTab *, const QPoint &)), this, SLOT(tabContextMenu(qsynthTab *, const QPoint &)));
}


// Kind of destructor.
void qsynthMainForm::destroy (void)
{
    // Stop the press!
    for (int i = 0; i < TabBar->count(); i++) {
        qsynthTab *pTab = (qsynthTab *) TabBar->tabAt(i);
        if (pTab) {
            qsynthEngine *pEngine = pTab->engine();
            if (pEngine) {
                if (!pEngine->isDefault())
                    m_pOptions->saveSetup(pEngine->setup(), pEngine->name());
                stopEngine(pEngine);
            }
        }
    }

    // No more options descriptor.
    m_pOptions = NULL;

    // Finally drop any popup widgets around...
    if (m_pMessagesForm)
        delete m_pMessagesForm;
    if (m_pChannelsForm)
        delete m_pChannelsForm;
}


// Make and set a proper setup step.
void qsynthMainForm::setup ( qsynthOptions *pOptions )
{
    // Finally, fix settings descriptor
    // and stabilize the form.
    m_pOptions = pOptions;

    // What style do we create these forms?
    WFlags wflags = Qt::WType_TopLevel;
    if (m_pOptions->bKeepOnTop)
        wflags |= Qt::WStyle_Tool;
    // All forms are to be created right now.
    m_pMessagesForm = new qsynthMessagesForm(this, 0, wflags);
    m_pChannelsForm = new qsynthChannelsForm(this, 0, wflags);

    // Get the default setup and dummy instace tab.
    TabBar->addTab(new qsynthTab(new qsynthEngine(m_pOptions)));
    // And all additional custom ones...
    for (QStringList::Iterator iter = m_pOptions->engines.begin(); iter != m_pOptions->engines.end(); iter++)
        TabBar->addTab(new qsynthTab(new qsynthEngine(m_pOptions, *iter)));

    // Try to restore old window positioning.
    m_pOptions->loadWidgetGeometry(this);
    // And for the whole widget gallore...
    m_pOptions->loadWidgetGeometry(m_pMessagesForm);
    m_pOptions->loadWidgetGeometry(m_pChannelsForm);

    // Set defaults...
    updateMessagesFont();
    updateMessagesLimit();

    // Check if we can redirect our own stdout/stderr...
    if (m_pOptions->bStdoutCapture && ::pipe(g_fdStdout) == 0) {
        ::dup2(g_fdStdout[QSYNTH_FDWRITE], STDOUT_FILENO);
        ::dup2(g_fdStdout[QSYNTH_FDWRITE], STDERR_FILENO);
        m_pStdoutNotifier = new QSocketNotifier(g_fdStdout[QSYNTH_FDREAD], QSocketNotifier::Read, this);
        QObject::connect(m_pStdoutNotifier, SIGNAL(activated(int)), this, SLOT(stdoutNotifySlot(int)));
    }

    // We'll accept drops from now on...
    setAcceptDrops(true);
    // Final startup stabilization...
    stabilizeForm();

    // Register the initial timer slot.
    QTimer::singleShot(QSYNTH_TIMER_MSECS, this, SLOT(timerSlot()));

}


// Window close event handlers.
bool qsynthMainForm::queryClose (void)
{
    bool bQueryClose = true;

    // Now's the time?
    if (m_pOptions) {
        // Dow we quit right away?
        if (m_pOptions->bQueryClose) {
            for (int i = 0; i < TabBar->count(); i++) {
                qsynthTab *pTab = (qsynthTab *) TabBar->tabAt(i);
                if (pTab) {
                    qsynthEngine *pEngine = pTab->engine();
                    if (pEngine && pEngine->pSynth) {
                        bQueryClose = (QMessageBox::warning(this, tr("Warning"),
                            QSYNTH_TITLE " " + tr("is about to terminate.") + "\n\n" +
                            tr("Are you sure?"),
                            tr("OK"), tr("Cancel")) == 0);
                        break;
                    }
                }
            }
        }
        // Some windows default fonts is here on demeand too.
        if (bQueryClose && m_pMessagesForm)
            m_pOptions->sMessagesFont = m_pMessagesForm->messagesFont().toString();
        // Try to save current positioning.
        if (bQueryClose) {
            m_pOptions->saveWidgetGeometry(m_pChannelsForm);
            m_pOptions->saveWidgetGeometry(m_pMessagesForm);
            m_pOptions->saveWidgetGeometry(this);
            // Close popup widgets.
            m_pMessagesForm->close();
            m_pChannelsForm->close();        
        }
    }
    return bQueryClose;
}


void qsynthMainForm::closeEvent ( QCloseEvent *pCloseEvent )
{
    if (queryClose())
        pCloseEvent->accept();
    else
        pCloseEvent->ignore();
}


// Add dropped files to playlist or soundfont stack.
void qsynthMainForm::playLoadFiles ( qsynthEngine *pEngine, const QStringList& files, bool bSetup )
{
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    qsynthSetup *pSetup = pEngine->setup();
    if (pSetup == NULL)
        return;

    // Add each list item to Soundfont stack or MIDI player playlist...
    const QString sPrefix  = pEngine->name() + ": ";
    const QString sElipsis = "...";
    int   iSoundfonts = 0;
    int   iMidiFiles  = 0;
    for (QStringList::ConstIterator iter = files.begin(); iter != files.end(); iter++) {
        char *pszFilename = (char *) (*iter).latin1();
        // Is it a soundfont file...
        if (::fluid_is_soundfont(pszFilename)) {
            if (bSetup || pSetup->soundfonts.find(*iter) == pSetup->soundfonts.end()) {
                appendMessagesColor(sPrefix + tr("Loading soundfont") + ": \"" + *iter + "\"" + sElipsis, "#999933");
                if (::fluid_synth_sfload(pEngine->pSynth, pszFilename, 1) >= 0) {
                    iSoundfonts++;
                    if (!bSetup)
                        pSetup->soundfonts.append(*iter);
                }
                else appendMessagesError(sPrefix + tr("Failed to load the soundfont") + ": \"" + *iter + "\".");
            }
        }  // Or is it a bare midifile?
        else if (::fluid_is_midifile(pszFilename) && pEngine->pPlayer) {
            appendMessagesColor(sPrefix + tr("Playing MIDI file") + ": \"" + *iter + "\"" + sElipsis, "#99cc66");
            if (::fluid_player_add(pEngine->pPlayer, pszFilename) >= 0)
                iMidiFiles++;
            else
                appendMessagesError(sPrefix + tr("Failed to play MIDI file") + ": \"" + *iter + "\".");
        }
    }
    
    // Reset all presets, if applicable...
    if (!bSetup && iSoundfonts > 0)
        resetEngine(pEngine);
    // Start playing, if any...
    if (pEngine->pPlayer && iMidiFiles > 0)
        ::fluid_player_play(pEngine->pPlayer);
}


// Drag'n'drop midi player feature handlers.
bool qsynthMainForm::decodeDragFiles ( const QMimeSource *pEvent, QStringList& files )
{
    bool bDecode = false;
    
    if (QTextDrag::canDecode(pEvent)) {
        QString sText;
        bDecode = QTextDrag::decode(pEvent, sText);
        if (bDecode) {
            files = QStringList::split("\n", sText);
            for (QStringList::Iterator iter = files.begin(); iter != files.end(); iter++)
                *iter = (*iter).stripWhiteSpace().replace(QRegExp("^file:"), "");
        }
    }
    
    return bDecode;
}

void qsynthMainForm::dragEnterEvent ( QDragEnterEvent* pDragEnterEvent )
{
    bool bAccept = false;
    
    if (QTextDrag::canDecode(pDragEnterEvent)) {
        QStringList files;
        if (decodeDragFiles(pDragEnterEvent, files)) {
            for (QStringList::Iterator iter = files.begin(); iter != files.end() && !bAccept; iter++) {
                char *pszFilename = (char *) (*iter).latin1();
                if (::fluid_is_midifile(pszFilename) || ::fluid_is_soundfont(pszFilename))
                    bAccept = true;
            }
        }
    }
    
    pDragEnterEvent->accept(bAccept);
}

void qsynthMainForm::dropEvent ( QDropEvent* pDropEvent )
{
    QStringList files;
    if (decodeDragFiles(pDropEvent, files))
        playLoadFiles(currentEngine(), files, false);
}


// Own stdout/stderr socket notifier slot.
void qsynthMainForm::stdoutNotifySlot ( int fd )
{
    char achBuffer[1024];
    int  cchBuffer = ::read(fd, achBuffer, sizeof(achBuffer) - 1);
    if (cchBuffer > 0) {
        achBuffer[cchBuffer] = (char) 0;
        appendStdoutBuffer(achBuffer);
    }
}


// Stdout buffer handler -- now splitted by complete new-lines...
void qsynthMainForm::appendStdoutBuffer ( const QString& s )
{
    m_sStdoutBuffer.append(s);

    int iLength = m_sStdoutBuffer.findRev('\n') + 1;
    if (iLength > 0) {
        QString sTemp = m_sStdoutBuffer.left(iLength);
        m_sStdoutBuffer.remove(0, iLength);
        QStringList list = QStringList::split('\n', sTemp, true);
        for (QStringList::Iterator iter = list.begin(); iter != list.end(); iter++)
            appendMessagesText(*iter);
    }
}


// Stdout flusher -- show up any unfinished line...
void qsynthMainForm::flushStdoutBuffer (void)
{
    if (!m_sStdoutBuffer.isEmpty()) {
        appendMessagesText(m_sStdoutBuffer);
        m_sStdoutBuffer.truncate(0);
    }
}


// Messages output methods.
void qsynthMainForm::appendMessages( const QString& s )
{
    if (m_pMessagesForm)
        m_pMessagesForm->appendMessages(s);
}

void qsynthMainForm::appendMessagesColor( const QString& s, const QString& c )
{
    if (m_pMessagesForm)
        m_pMessagesForm->appendMessagesColor(s, c);
}

void qsynthMainForm::appendMessagesText( const QString& s )
{
    if (m_pMessagesForm)
        m_pMessagesForm->appendMessagesText(s);
}

void qsynthMainForm::appendMessagesError( const QString& s )
{
    if (m_pMessagesForm)
        m_pMessagesForm->show();

    appendMessagesColor(s, "#ff0000");

    QMessageBox::critical(this, tr("Error"), s, tr("Cancel"));
}


// Force update of the messages font.
void qsynthMainForm::updateMessagesFont (void)
{
    if (m_pOptions == NULL)
        return;

    if (m_pMessagesForm && !m_pOptions->sMessagesFont.isEmpty()) {
        QFont font;
        if (font.fromString(m_pOptions->sMessagesFont))
            m_pMessagesForm->setMessagesFont(font);
    }
}


// Update messages window line limit.
void qsynthMainForm::updateMessagesLimit (void)
{
    if (m_pOptions == NULL)
        return;

    if (m_pMessagesForm) {
        if (m_pOptions->bMessagesLimit)
            m_pMessagesForm->setMessagesLimit(m_pOptions->iMessagesLimitLines);
        else
            m_pMessagesForm->setMessagesLimit(0);
    }
}


// Stabilize current form toggle buttons that may be astray.
void qsynthMainForm::stabilizeForm (void)
{
    qsynthEngine *pEngine = currentEngine();
    
    bool bEnabled = (pEngine && pEngine->pSynth);
    
    GainGroupBox->setEnabled(bEnabled);
    ReverbGroupBox->setEnabled(bEnabled);
    ChorusGroupBox->setEnabled(bEnabled);
    OutputGroupBox->setEnabled(bEnabled);
    ProgramResetPushButton->setEnabled(bEnabled);
    SystemResetPushButton->setEnabled(bEnabled);
    
    if (bEnabled) {
        bool bReverbActive = ReverbActiveCheckBox->isChecked();
        ReverbRoomTextLabel->setEnabled(bReverbActive);
        ReverbDampTextLabel->setEnabled(bReverbActive);
        ReverbWidthTextLabel->setEnabled(bReverbActive);
        ReverbLevelTextLabel->setEnabled(bReverbActive);
        ReverbRoomDial->setEnabled(bReverbActive);
        ReverbDampDial->setEnabled(bReverbActive);
        ReverbWidthDial->setEnabled(bReverbActive);
        ReverbLevelDial->setEnabled(bReverbActive);
        ReverbRoomSpinBox->setEnabled(bReverbActive);
        ReverbDampSpinBox->setEnabled(bReverbActive);
        ReverbWidthSpinBox->setEnabled(bReverbActive);
        ReverbLevelSpinBox->setEnabled(bReverbActive);
        bool bChorusActive = ChorusActiveCheckBox->isChecked();
        ChorusNrTextLabel->setEnabled(bChorusActive);
        ChorusLevelTextLabel->setEnabled(bChorusActive);
        ChorusSpeedTextLabel->setEnabled(bChorusActive);
        ChorusDepthTextLabel->setEnabled(bChorusActive);
        ChorusTypeTextLabel->setEnabled(bChorusActive);
        ChorusNrDial->setEnabled(bChorusActive);
        ChorusLevelDial->setEnabled(bChorusActive);
        ChorusSpeedDial->setEnabled(bChorusActive);
        ChorusDepthDial->setEnabled(bChorusActive);
        ChorusNrSpinBox->setEnabled(bChorusActive);
        ChorusLevelSpinBox->setEnabled(bChorusActive);
        ChorusSpeedSpinBox->setEnabled(bChorusActive);
        ChorusDepthSpinBox->setEnabled(bChorusActive);
        ChorusTypeComboBox->setEnabled(bChorusActive);
        qsynthSetup *pSetup = pEngine->setup();
        bool bMidiIn = (pSetup && pSetup->bMidiIn);
        ChannelsPushButton->setEnabled(bMidiIn);
        RestartPushButton->setText(tr("Re&start"));
    } else {
        ChannelsPushButton->setEnabled(false);
        RestartPushButton->setText(tr("&Start"));
    }
    RestartPushButton->setEnabled(true);

    MessagesPushButton->setOn(m_pMessagesForm && m_pMessagesForm->isVisible());
    ChannelsPushButton->setOn(m_pChannelsForm && m_pChannelsForm->isVisible());
}


// Program reset command slot (all channels).
void qsynthMainForm::programReset (void)
{
    ProgramResetPushButton->setEnabled(false);
    
    resetEngine(currentEngine());
    if (m_pChannelsForm)
        m_pChannelsForm->resetAllChannels(true);
    stabilizeForm();
}


// System reset command slot.
void qsynthMainForm::systemReset (void)
{
    SystemResetPushButton->setEnabled(false);
    
    qsynthEngine *pEngine = currentEngine();
    if (pEngine && pEngine->pSynth) {
#ifdef CONFIG_FLUID_RESET
        appendMessagesColor(pEngine->name() + ": fluid_synth_system_reset()", "#993366");
        ::fluid_synth_system_reset(pEngine->pSynth);
#else
        appendMessagesColor(pEngine->name() + ": fluid_synth_program_reset()", "#996666");
        ::fluid_synth_program_reset(pEngine->pSynth);
#endif
        if (m_pChannelsForm)
            m_pChannelsForm->resetAllChannels(true);
    }
    stabilizeForm();
}


// Complete engine restart.
void qsynthMainForm::promptRestart (void)
{
    RestartPushButton->setEnabled(false);
    restartEngine(currentEngine());
}


// Prompt and create a new engine instance.
bool qsynthMainForm::newEngineTab (void)
{
    qsynthEngine *pEngine;
    QString sName;
    
    // Simple hack for finding a unused engine name...
    const QString sPrefix = QSYNTH_TITLE;
    int   iSuffix = TabBar->count() + 1;    // One is always there, so try after...
    bool  bRetry  = true;
    while (bRetry) {
        sName  = sPrefix + QString::number(iSuffix++);
        bRetry = false;
        for (int i = 0; i < TabBar->count() && !bRetry; i++) {
            qsynthTab *pTab = (qsynthTab *) TabBar->tabAt(i);
            if (pTab) {
                pEngine = pTab->engine();
                if (pEngine->name() == sName)
                    bRetry = true;
            }
        }
    }

    // Probably a good idea to prompt for the setup dialog.
    pEngine = new qsynthEngine(m_pOptions, sName);
    bool bResult = setupEngineTab(pEngine, NULL);
    if (bResult) {
        // Success, add a new tab...
        TabBar->addTab(new qsynthTab(pEngine));
        // And try to be persistent...
        m_pOptions->newEngine(pEngine);
        // Update bar...
        TabBar->update();
    } else {
        // As this will not be mangaed by a qsynthTab instance,
        // we better free it up right now...
        delete pEngine;
    }
    // Done.
    return bResult;
}


// Delete and engine instance.
bool qsynthMainForm::deleteEngineTab ( qsynthEngine *pEngine, qsynthTab *pTab )
{
    if (pEngine == NULL || pTab == NULL)
        return false;

    // Try to prompt user if he/she really wants this...
    bool bResult = (QMessageBox::warning(this, tr("Warning"),
        tr("Delete fluidsynth engine:") + "\n\n" +
        pEngine->name() + "\n\n" +
        tr("Are you sure?"),
        tr("OK"), tr("Cancel")) == 0);
    
    if (bResult) {
        // First we try to stop the angine.
        stopEngine(pEngine);
        // Better nullify the current reference, if applicable.
        if (g_pCurrentEngine == pEngine)
            g_pCurrentEngine = NULL;
        // Nows time to remove those crappy entries...
        m_pOptions->deleteEngine(pEngine);
        // Then, we delete the instance (note that the engine object
        // is owned by the tab instance, so it will be delete here).
        TabBar->removeTab(pTab);
        TabBar->update();
        tabSelect(TabBar->currentTab());
    }
    
    return bResult;
}


// Edit settings of a given engine instance.
bool qsynthMainForm::setupEngineTab ( qsynthEngine *pEngine, qsynthTab *pTab )
{
    bool bResult = false;

    if (pEngine == NULL || pEngine->setup() == NULL)
        return bResult;

    qsynthSetupForm *pSetupForm = new qsynthSetupForm(this);
    if (pSetupForm) {
        // Load the current instance settings.
        pSetupForm->setup(m_pOptions, pEngine, (pTab == NULL));
        // Show the instance setup dialog, then ask for a engine restart?
        bResult = pSetupForm->exec();
        if (bResult) {
            // Have we changed names? Ugly uh?
            if (pTab && m_pOptions->renameEngine(pEngine)) {
                // Update main caption, if we're on current engine tab...
                if (pTab->identifier() == TabBar->currentTab())
                    setCaption(QSYNTH_TITLE " - " + tr(QSYNTH_SUBTITLE) + " [" + pEngine->name() + "]");
                // Finally update tab text...
                pTab->setText(pEngine->name());
            }
            // Now we may restart this.
            restartEngine(pEngine);
        }
        // Done.
        delete pSetupForm;
    }

    return bResult;
}


// Message log form requester slot.
void qsynthMainForm::toggleMessagesForm (void)
{
    if (m_pOptions == NULL)
        return;

    if (m_pMessagesForm) {
        m_pOptions->saveWidgetGeometry(m_pMessagesForm);
        if (m_pMessagesForm->isVisible()) {
            m_pMessagesForm->hide();
        } else {
            m_pMessagesForm->show();
            m_pMessagesForm->raise();
            m_pMessagesForm->setActiveWindow();
        }
    }
}


// Channels view form requester slot.
void qsynthMainForm::toggleChannelsForm (void)
{
    if (m_pOptions == NULL)
        return;

    if (m_pChannelsForm) {
        m_pOptions->saveWidgetGeometry(m_pChannelsForm);
        if (m_pChannelsForm->isVisible()) {
            m_pChannelsForm->hide();
        } else {
            m_pChannelsForm->show();
            m_pChannelsForm->raise();
            m_pChannelsForm->setActiveWindow();
        }
    }
}


// Instance dialog requester slot.
void qsynthMainForm::showSetupForm (void)
{
    qsynthTab *pTab = (qsynthTab *) TabBar->tab(TabBar->currentTab());
    if (pTab)
        setupEngineTab(pTab->engine(), pTab);
}


// Setup dialog requester slot.
void qsynthMainForm::showOptionsForm (void)
{
    if (m_pOptions == NULL)
        return;

    qsynthOptionsForm *pOptionsForm = new qsynthOptionsForm(this);
    if (pOptionsForm) {
        // Check out some initial nullities(tm)...
        if (m_pOptions->sMessagesFont.isEmpty() && m_pMessagesForm)
            m_pOptions->sMessagesFont = m_pMessagesForm->messagesFont().toString();
        // To track down deferred or immediate changes.
        QString sOldMessagesFont = m_pOptions->sMessagesFont;
        bool    bStdoutCapture   = m_pOptions->bStdoutCapture;
        bool    bKeepOnTop       = m_pOptions->bKeepOnTop;
        int     bMessagesLimit   = m_pOptions->bMessagesLimit;
        int     iMessagesLimitLines = m_pOptions->iMessagesLimitLines;
        // Load the current setup settings.
        pOptionsForm->setup(m_pOptions);
        // Show the setup dialog...
        if (pOptionsForm->exec()) {
            // Warn if something will be only effective on next run.
            if (( bStdoutCapture && !m_pOptions->bStdoutCapture) ||
                (!bStdoutCapture &&  m_pOptions->bStdoutCapture) ||
                ( bKeepOnTop     && !m_pOptions->bKeepOnTop)     ||
                (!bKeepOnTop     &&  m_pOptions->bKeepOnTop)) {
                QMessageBox::information(this, tr("Information"),
                    tr("Some settings will be only effective\n"
                       "next time you start this program."), tr("OK"));
            }
            // Check wheather something immediate has changed.
            if (sOldMessagesFont != m_pOptions->sMessagesFont)
                updateMessagesFont();
            if (( bMessagesLimit && !m_pOptions->bMessagesLimit) ||
                (!bMessagesLimit &&  m_pOptions->bMessagesLimit) ||
                (iMessagesLimitLines !=  m_pOptions->iMessagesLimitLines))
                updateMessagesLimit();
        }
        // Done.
        delete pOptionsForm;
    }
}


// About dialog requester slot.
void qsynthMainForm::showAboutForm (void)
{
    qsynthAboutForm *pAboutForm = new qsynthAboutForm(this);
    if (pAboutForm) {
        pAboutForm->exec();
        delete pAboutForm;
    }
}


// Tab selection slot.
void qsynthMainForm::tabSelect ( int iTab )
{
    if (iTab == m_iCurrentTab)
        return;

    const QString sElipsis = "...";
    const QString sColon   = ": ";

    // Try to save old tab settings...
    qsynthTab *pTab = (qsynthTab *) TabBar->tab(m_iCurrentTab);
    if (pTab) {
        qsynthEngine *pEngine = pTab->engine();
        if (pEngine) {
            appendMessages(pEngine->name() + sColon + tr("Saving panel settings") + sElipsis);
            savePanelSettings(pEngine);
        }
    }
    
    // Make it official.
    m_iCurrentTab = iTab;

    // And set new ones, while refreshing views...
    pTab = (qsynthTab *) TabBar->tab(m_iCurrentTab);
    if (pTab) {
        qsynthEngine *pEngine = pTab->engine();
        if (pEngine) {
            // Set current engine reference hack.
            g_pCurrentEngine = pEngine;
            // And do the change.
            setCaption(QSYNTH_TITLE " - " + tr(QSYNTH_SUBTITLE) + " [" + pEngine->name() + "]");
            appendMessages(pEngine->name() + sColon + tr("Loading panel settings") + sElipsis);
            loadPanelSettings(pEngine, false);
            resetChannelsForm(pEngine, false);
        }
    }

    // Finally, stabilize main form.
    stabilizeForm();
}


// Common context request slot.
void qsynthMainForm::tabContextMenu ( qsynthTab *pTab, const QPoint& pos )
{
    qsynthEngine *pEngine = NULL;
    if (pTab)
        pEngine = pTab->engine();

    QPopupMenu* pContextMenu = new QPopupMenu(this);
    int iNew    = pContextMenu->insertItem(tr("New..."));
    int iDelete = pContextMenu->insertItem(tr("Delete"));
    pContextMenu->setItemEnabled(iDelete, pEngine && !pEngine->isDefault());
    pContextMenu->insertSeparator();
    int iSetup  = pContextMenu->insertItem(tr("Setup..."));
    pContextMenu->setItemEnabled(iSetup, pEngine);
    int iItemID = pContextMenu->exec(pos);
    delete pContextMenu;

    if (iItemID == iNew)
        newEngineTab();
    else if (iItemID == iDelete)
        deleteEngineTab(pEngine, pTab);
    else if (iItemID == iSetup)
        setupEngineTab(pEngine, pTab);
}


// Timer callback funtion.
void qsynthMainForm::timerSlot (void)
{
    // Is it the first shot on synth start after a one slot delay?
    if (m_iTimerDelay < QSYNTH_DELAY_MSECS) {
        m_iTimerDelay += QSYNTH_TIMER_MSECS;
        if (m_iTimerDelay >= QSYNTH_DELAY_MSECS) {
            // Start the press!
            for (int i = 0; i < TabBar->count(); i++) {
                qsynthTab *pTab = (qsynthTab *) TabBar->tabAt(i);
                if (pTab)
                    startEngine(pTab->engine());
            }
        }
    }

    // Some global MIDI activity?
    int iTabUpdate = 0;
    for (int i = 0; i < TabBar->count(); i++) {
        qsynthTab *pTab = (qsynthTab * ) TabBar->tabAt(i);
        if (pTab) {
            qsynthEngine *pEngine = pTab->engine();
            if (pEngine->iMidiEvent > 0) {
                pEngine->iMidiEvent = 0;
                if (pEngine->iMidiState == 0) {
                    pEngine->iMidiState++;
                    pTab->setOn(true);
                    iTabUpdate++;
                }
            }
            else if (pEngine->iMidiEvent == 0 && pEngine->iMidiState > 0) {
                pEngine->iMidiState--;
                pTab->setOn(false);
                iTabUpdate++;
            }
        }
    }
    // Do we have an update?
    if (iTabUpdate > 0)
        TabBar->update();

    // MIDI Channel activity breakout...
    if (m_pChannelsForm) {
        for (int iChan = 0; iChan < g_iMidiChannels; iChan++) {
            if (g_pMidiChannels[iChan].iEvent > 0) {
                g_pMidiChannels[iChan].iEvent = 0;
                // Activity tracking...
                if (g_pMidiChannels[iChan].iState == 0) {
                    m_pChannelsForm->setChannelOn(iChan, true);
                    g_pMidiChannels[iChan].iState++;
                }
                // Control and/or program change...
                if (g_pMidiChannels[iChan].iChange > 0) {
                    g_pMidiChannels[iChan].iChange = 0;
                    m_pChannelsForm->updateChannel(iChan);
                }
            }   // Activity fallback...
            else if (g_pMidiChannels[iChan].iEvent == 0 && g_pMidiChannels[iChan].iState > 0) {
                m_pChannelsForm->setChannelOn(iChan, false);
                g_pMidiChannels[iChan].iState--;
            }
        }
    }

    // Gain changes?
    if (m_iGainChanged > 0)
        updateGain();

    // Reverb changes?
    if (m_iReverbChanged > 0)
        updateReverb();

    // Chorus changes?
    if (m_iChorusChanged > 0)
        updateChorus();

    // Meter update.
    OutputMeter->setValue(0, g_pCurrentEngine->fMeterValue[0]);
    OutputMeter->setValue(1, g_pCurrentEngine->fMeterValue[1]);
    OutputMeter->refresh();
    g_pCurrentEngine->fMeterValue[0] = 0.0;
    g_pCurrentEngine->fMeterValue[1] = 0.0;

    // Register for the next timer slot.
    QTimer::singleShot(QSYNTH_TIMER_MSECS, this, SLOT(timerSlot()));
}


// Return the current selected engine.
qsynthEngine *qsynthMainForm::currentEngine (void)
{
    return g_pCurrentEngine;
}


// Start the fluidsynth clone, based on given settings.
bool qsynthMainForm::startEngine ( qsynthEngine *pEngine )
{
    if (pEngine == NULL)
        return false;
    if (pEngine->pSynth)
        return true;

    qsynthSetup *pSetup = pEngine->setup();
    if (pSetup == NULL)
        return false;

    // Start realizing settings...
    pSetup->realize();

    const QString sPrefix  = pEngine->name() + ": ";
    const QString sElipsis = "...";
    QStringList::Iterator iter;

    // Create the synthesizer.
    appendMessages(sPrefix + tr("Creating synthesizer engine") + sElipsis);
    pEngine->pSynth = ::new_fluid_synth(pSetup->fluid_settings());
    if (pEngine->pSynth == NULL) {
        appendMessagesError(sPrefix + tr("Failed to create the synthesizer. Cannot continue without it."));
        return false;
    }

    // Load the soundfonts...
    playLoadFiles(pEngine, pSetup->soundfonts, true);

    // Start the synthesis thread...
    appendMessages(sPrefix + tr("Creating audio driver") + " (" + pSetup->sAudioDriver + ")" + sElipsis);
//  pEngine->pAudioDriver = ::new_fluid_audio_driver(pSetup->fluid_settings(), pEngine->pSynth);
    pEngine->pAudioDriver = ::new_fluid_audio_driver2(pSetup->fluid_settings(), qsynth_process, pEngine);
    if (pEngine->pAudioDriver == NULL) {
        appendMessagesError(sPrefix + tr("Failed to create the audio driver") + " (" + pSetup->sAudioDriver + "). " + tr("Cannot continue without it."));
        stopEngine(pEngine);
        return false;
    }

    // Start the midi router and link it to the synth...
    if (pSetup->bMidiIn) {
        // In dump mode, text output is generated for events going into
        // and out of the router. The example dump functions are put into
        // the chain before and after the router..
        appendMessages(sPrefix + tr("Creating MIDI router") + " (" + pSetup->sMidiDriver + ")" + sElipsis);
        pEngine->pMidiRouter = ::new_fluid_midi_router(pSetup->fluid_settings(),
            pSetup->bMidiDump ? qsynth_dump_postrouter : qsynth_handle_midi_event,
            (void *) pEngine);
        if (pEngine->pMidiRouter == NULL) {
            appendMessagesError(sPrefix + tr("Failed to create the MIDI input router") + " (" + pSetup->sMidiDriver + "); " + tr("no MIDI input will be available."));
        } else {
            ::fluid_synth_set_midi_router(pEngine->pSynth, pEngine->pMidiRouter);
            appendMessages(sPrefix + tr("Creating MIDI driver") + " (" + pSetup->sMidiDriver + ")" + sElipsis);
            pEngine->pMidiDriver = ::new_fluid_midi_driver(pSetup->fluid_settings(),
                pSetup->bMidiDump ? ::fluid_midi_dump_prerouter : ::fluid_midi_router_handle_midi_event,
               (void *) pEngine->pMidiRouter);
            if (pEngine->pMidiDriver == NULL)
                appendMessagesError(sPrefix + tr("Failed to create the MIDI driver") + " (" + pSetup->sMidiDriver + "); " + tr("no MIDI input will be available."));
        }
    }

    // Create the MIDI player.
    appendMessages(sPrefix + tr("Creating MIDI player") + sElipsis);
    pEngine->pPlayer = ::new_fluid_player(pEngine->pSynth);
    if (pEngine->pPlayer == NULL) {
        appendMessagesError(sPrefix + tr("Failed to create the MIDI player. Continuing without a player."));
    } else {
        // Play the midi files, if any.
        playLoadFiles(pEngine, pSetup->midifiles, false);
    }

    // Run the server, if requested.
    if (pSetup->bServer) {
#ifdef CONFIG_FLUID_SERVER
        appendMessages(sPrefix + tr("Creating server") + sElipsis);
        pEngine->pServer = ::new_fluid_server(pSetup->fluid_settings(), qsynth_newclient, pEngine->pSynth);
        if (pEngine->pServer == NULL)
            appendMessagesError(sPrefix + tr("Failed to create the server. Continuing without it."));
#else
        appendMessagesError(sPrefix + tr("Server mode disabled. Continuing without it."));
#endif
    }

    // Make an initial program reset.
    resetEngine(pEngine);

    // Show up our efforts, if we're currently selected :)
    if (pEngine == currentEngine()) {
        loadPanelSettings(pEngine, true);
        resetChannelsForm(pEngine, true);
        stabilizeForm();
    } else {
        setEngineReverbOn(pEngine, pSetup->bReverbActive);
        setEngineChorusOn(pEngine, pSetup->bChorusActive);
        setEngineGain(pEngine, pSetup->fGain);
        setEngineReverb(pEngine, pSetup->fReverbRoom, pSetup->fReverbDamp, pSetup->fReverbWidth, pSetup->fReverbLevel);
        setEngineChorus(pEngine, pSetup->iChorusNr, pSetup->fChorusLevel, pSetup->fChorusSpeed, pSetup->fChorusDepth, pSetup->iChorusType);
    }
    
    // All is right.
    appendMessages(sPrefix + tr("Synthesizer engine started."));

    return true;
}


// Stop the fluidsynth clone.
void qsynthMainForm::stopEngine ( qsynthEngine *pEngine )
{
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    qsynthSetup *pSetup = pEngine->setup();
    if (pSetup == NULL)
        return;

    // Flush anything that maybe pending...
    flushStdoutBuffer();

    const QString sPrefix  = pEngine->name() + ": ";
    const QString sElipsis = "...";

#ifdef CONFIG_FLUID_SERVER
    // Destroy server.
    if (pEngine->pServer) {
        appendMessages(sPrefix + tr("Waiting for server to terminate") + sElipsis);
        ::fluid_server_join(pEngine->pServer);
        appendMessages(sPrefix + tr("Destroying server") + sElipsis);
        ::delete_fluid_server(pEngine->pServer);
        pEngine->pServer = NULL;
    }
#endif

    // Destroy player.
    if (pEngine->pPlayer) {
        appendMessages(sPrefix + tr("Stopping MIDI player") + sElipsis);
        ::fluid_player_stop(pEngine->pPlayer);
        appendMessages(sPrefix + tr("Waiting for MIDI player to terminate") + sElipsis);
        ::fluid_player_join(pEngine->pPlayer);
        appendMessages(sPrefix + tr("Destroying MIDI player") + sElipsis);
        ::delete_fluid_player(pEngine->pPlayer);
        pEngine->pPlayer = NULL;
    }

    // Destroy MIDI router.
    if (pEngine->pMidiRouter) {
        if (pEngine->pMidiDriver) {
            appendMessages(sPrefix + tr("Destroying MIDI driver") + sElipsis);
            ::delete_fluid_midi_driver(pEngine->pMidiDriver);
            pEngine->pMidiDriver = NULL;
        }
        appendMessages(sPrefix + tr("Destroying MIDI router") + sElipsis);
        ::delete_fluid_midi_router(pEngine->pMidiRouter);
        pEngine->pMidiRouter = NULL;
    }

    // Destroy audio driver.
    if (pEngine->pAudioDriver) {
        appendMessages(sPrefix + tr("Destroying audio driver") + sElipsis);
        ::delete_fluid_audio_driver(pEngine->pAudioDriver);
        pEngine->pAudioDriver = NULL;
    }

    // And finally, destroy the synthesizer engine.
    if (pEngine->pSynth) {
        appendMessages(sPrefix + tr("Destroying synthesizer engine") + sElipsis);
        ::delete_fluid_synth(pEngine->pSynth);
        pEngine->pSynth = NULL;
        // We're done.
        appendMessages(sPrefix + tr("Synthesizer engine terminated."));
    }

    // Show up our efforts, if we're currently selected :p
    if (pEngine == currentEngine()) {
        savePanelSettings(pEngine);
        resetChannelsForm(pEngine, true);
        stabilizeForm();
    }

   // Wait a litle bit before continue...
    QTime t;
    t.start();
    while (t.elapsed() < QSYNTH_DELAY_MSECS)
        QApplication::eventLoop()->processEvents(QEventLoop::ExcludeUserInput);
}


// Start the fluidsynth clone, based on given settings.
void qsynthMainForm::restartEngine ( qsynthEngine *pEngine )
{
    bool bRestart = true;
    
    // If currently running, prompt user...
    if (pEngine && pEngine->pSynth) {
        bRestart = (QMessageBox::warning(this, tr("Warning"),
            tr("New settings will be effective after\n"
               "restarting the fluidsynth engine:") + "\n\n" +
            pEngine->name() + "\n\n" +
            tr("Please note that this operation may cause\n"
               "temporary MIDI and Audio disruption.") + "\n\n" +
            tr("Do you want to restart the engine now?"),
            tr("Yes"), tr("No")) == 0);
    }

    // If allowed, just restart the engine...
    if (bRestart) {
        // Restarting means stopping current engine...
        stopEngine(pEngine);
        // And making room for immediate restart...
        m_iTimerDelay = 0;
    }
}


// Engine reset (all channels program reset).
void qsynthMainForm::resetEngine ( qsynthEngine *pEngine )
{
    if (pEngine && pEngine->pSynth) {
        appendMessagesColor(pEngine->name() + ": fluid_synth_program_reset()", "#996666");
        ::fluid_synth_program_reset(pEngine->pSynth);
    }
}


// Engine gain settings.
void qsynthMainForm::setEngineGain ( qsynthEngine *pEngine, float fGain )
{
    appendMessagesColor(pEngine->name() + ": fluid_synth_set_gain(" + QString::number(fGain) + ")", "#6699cc");

    ::fluid_synth_set_gain(pEngine->pSynth, fGain);
}


// Engine reverb settings.
void qsynthMainForm::setEngineReverbOn ( qsynthEngine *pEngine, bool bActive )
{
    appendMessagesColor(pEngine->name() + ": fluid_synth_set_reverb_on(" + QString::number((int) bActive) + ")", "#99cc33");

    ::fluid_synth_set_reverb_on(pEngine->pSynth, (int) bActive);
}

void qsynthMainForm::setEngineReverb ( qsynthEngine *pEngine, float fRoom, float fDamp, float fWidth, float fLevel )
{
    appendMessagesColor(pEngine->name() + ": fluid_synth_set_reverb("
        + QString::number(fRoom)  + ","
        + QString::number(fDamp)  + ","
        + QString::number(fWidth) + ","
        + QString::number(fLevel) + ")", "#99cc66");

    ::fluid_synth_set_reverb(pEngine->pSynth, fRoom, fDamp, fWidth, fLevel);
}


// Engine chorus settings.
void qsynthMainForm::setEngineChorusOn ( qsynthEngine *pEngine, bool bActive )
{
    appendMessagesColor(pEngine->name() + ": fluid_synth_set_chorus_on(" + QString::number((int) bActive) + ")", "#cc9933");

    ::fluid_synth_set_chorus_on(pEngine->pSynth, (int) bActive);
}

void qsynthMainForm::setEngineChorus ( qsynthEngine *pEngine, int iNr, float fLevel, float fSpeed, float fDepth, int iType )
{
    appendMessagesColor(pEngine->name() + ": fluid_synth_set_chorus("
        + QString::number(iNr)    + ","
        + QString::number(fLevel) + ","
        + QString::number(fSpeed) + ","
        + QString::number(fDepth) + ","
        + QString::number(iType)  + ")", "#cc9966");

    ::fluid_synth_set_chorus(pEngine->pSynth, iNr, fLevel, fSpeed, fDepth, iType);
}


// Front panel state load routine.
void qsynthMainForm::loadPanelSettings ( qsynthEngine *pEngine, bool bUpdate )
{
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    qsynthSetup *pSetup = pEngine->setup();
    if (pSetup == NULL)
        return;

    // Reset change flags.
    m_iGainChanged   = 0;
    m_iReverbChanged = 0;
    m_iChorusChanged = 0;
    
    // Avoid update races: set update counters > 0)...
    m_iGainUpdated   = 1;
    m_iReverbUpdated = 1;
    m_iChorusUpdated = 1;

    GainDial->setValue(qsynth_iscale_clip(QSYNTH_GAIN_SCALE, pSetup->fGain));

    ReverbActiveCheckBox->setChecked(pSetup->bReverbActive);
    ReverbRoomDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, pSetup->fReverbRoom));
    ReverbDampDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, pSetup->fReverbDamp));
    ReverbWidthDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, pSetup->fReverbWidth));
    ReverbLevelDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, pSetup->fReverbLevel));

    ChorusActiveCheckBox->setChecked(pSetup->bChorusActive);
    ChorusNrDial->setValue(pSetup->iChorusNr);
    ChorusLevelDial->setValue(qsynth_iscale_clip(QSYNTH_CHORUS_SCALE, pSetup->fChorusLevel));
    ChorusSpeedDial->setValue(qsynth_iscale_clip(QSYNTH_CHORUS_SCALE, pSetup->fChorusSpeed));
    ChorusDepthDial->setValue(qsynth_iscale_clip(QSYNTH_CHORUS_SCALE, pSetup->fChorusDepth));
    ChorusTypeComboBox->setCurrentItem(pSetup->iChorusType);

    // Make them dirty.
    if (bUpdate) {
        m_iGainChanged++;
        m_iReverbChanged++;
        m_iChorusChanged++;
    }
    
    // Let them get updated, possibly on next tick.
    m_iGainUpdated   = 0;
    m_iReverbUpdated = 0;
    m_iChorusUpdated = 0;
}


// Front panel state save routine.
void qsynthMainForm::savePanelSettings ( qsynthEngine *pEngine )
{
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    qsynthSetup *pSetup = pEngine->setup();
    if (pSetup == NULL)
        return;

    pSetup->fGain = qsynth_fscale_clip(GainDial->value(), QSYNTH_GAIN_SCALE);

    pSetup->bReverbActive = ReverbActiveCheckBox->isChecked();
    pSetup->fReverbRoom   = qsynth_fscale_clip(ReverbRoomSpinBox->value(),  QSYNTH_REVERB_SCALE);
    pSetup->fReverbDamp   = qsynth_fscale_clip(ReverbDampSpinBox->value(),  QSYNTH_REVERB_SCALE);
    pSetup->fReverbWidth  = qsynth_fscale_clip(ReverbWidthSpinBox->value(), QSYNTH_REVERB_SCALE);
    pSetup->fReverbLevel  = qsynth_fscale_clip(ReverbLevelSpinBox->value(), QSYNTH_REVERB_SCALE);

    pSetup->bChorusActive = ChorusActiveCheckBox->isChecked();
    pSetup->iChorusNr     = ChorusNrSpinBox->value();
    pSetup->fChorusLevel  = qsynth_fscale_clip(ChorusLevelSpinBox->value(), QSYNTH_CHORUS_SCALE);
    pSetup->fChorusSpeed  = qsynth_fscale_clip(ChorusSpeedSpinBox->value(), QSYNTH_CHORUS_SCALE);
    pSetup->fChorusDepth  = qsynth_fscale_clip(ChorusDepthSpinBox->value(), QSYNTH_CHORUS_SCALE);
    pSetup->iChorusType   = ChorusTypeComboBox->currentItem();
}


// Complete refresh of the floating channels form.
void qsynthMainForm::resetChannelsForm ( qsynthEngine *pEngine, bool bPreset )
{
    if (m_pChannelsForm == NULL)
        return;
        
    // Setup the channels view window.
    m_pChannelsForm->setup(m_pOptions, pEngine, bPreset);
    
    // Reset the channel event state flaggers.
    if (g_pMidiChannels)
        delete [] g_pMidiChannels;
    g_pMidiChannels = NULL;
    g_iMidiChannels = 0;
    // Prepare the new channel event state flaggers.
    if (pEngine && pEngine->pSynth)
        g_iMidiChannels = ::fluid_synth_count_midi_channels(pEngine->pSynth);
    if (g_iMidiChannels > 0)
        g_pMidiChannels = new qsynth_midi_channel [g_iMidiChannels];
    if (g_pMidiChannels) {
        for (int iChan = 0; iChan < g_iMidiChannels; iChan++) {
            g_pMidiChannels[iChan].iEvent  = 0;
            g_pMidiChannels[iChan].iState  = 0;
            g_pMidiChannels[iChan].iChange = 0;
        }
    }
}


// Increment reverb change flag.
void qsynthMainForm::reverbActivate ( bool bActive )
{
    if (m_iReverbUpdated > 0)
        return;
        
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    setEngineReverbOn(pEngine, bActive);

    if (bActive)
        refreshReverb();

    stabilizeForm();
}


// Increment chorus change flag.
void qsynthMainForm::chorusActivate ( bool bActive )
{
    if (m_iChorusUpdated > 0)
        return;
    
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    setEngineChorusOn(pEngine, bActive);

    if (bActive)
        refreshChorus();

    stabilizeForm();
}


// Increment gain change flag.
void qsynthMainForm::gainChanged (int)
{
    if (m_iGainUpdated == 0)
        m_iGainChanged++;
}


// Increment reverb change flag.
void qsynthMainForm::reverbChanged (int)
{
    if (m_iReverbUpdated == 0)
        m_iReverbChanged++;
}


// Increment chorus change flag.
void qsynthMainForm::chorusChanged (int)
{
    if (m_iChorusUpdated == 0)
        m_iChorusChanged++;
}


// Update gain state.
void qsynthMainForm::updateGain (void)
{
    if (m_iGainUpdated > 0)
        return;
        
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;
    m_iGainUpdated++;

    float fGain = qsynth_fscale_clip(GainSpinBox->value(), QSYNTH_GAIN_SCALE);

    setEngineGain(pEngine, fGain);
    refreshGain();

    m_iGainUpdated--;
    m_iGainChanged = 0;
}


// Update reverb state.
void qsynthMainForm::updateReverb (void)
{
    if (m_iReverbUpdated > 0)
        return;
        
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;
    m_iReverbUpdated++;

    double fReverbRoom  = qsynth_fscale_clip(ReverbRoomSpinBox->value(), QSYNTH_REVERB_SCALE);
    double fReverbDamp  = qsynth_fscale_clip(ReverbDampSpinBox->value(), QSYNTH_REVERB_SCALE);
    double fReverbWidth = qsynth_fscale_clip(ReverbWidthSpinBox->value(), QSYNTH_REVERB_SCALE);
    double fReverbLevel = qsynth_fscale_clip(ReverbLevelSpinBox->value(), QSYNTH_REVERB_SCALE);

    setEngineReverb(pEngine, fReverbRoom, fReverbDamp, fReverbWidth, fReverbLevel);
    refreshReverb();

    m_iReverbUpdated--;
    m_iReverbChanged = 0;
}


// Update chorus state.
void qsynthMainForm::updateChorus (void)
{
    if (m_iChorusUpdated > 0)
        return;
    
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;
    m_iChorusUpdated++;

    int    iChorusNr    = ChorusNrSpinBox->value();
    double fChorusLevel = qsynth_fscale_clip(ChorusLevelSpinBox->value(), QSYNTH_CHORUS_SCALE);
    double fChorusSpeed = qsynth_fscale_clip(ChorusSpeedSpinBox->value(), QSYNTH_CHORUS_SCALE);
    double fChorusDepth = qsynth_fscale_clip(ChorusDepthSpinBox->value(), QSYNTH_CHORUS_SCALE);
    int    iChorusType  = ChorusTypeComboBox->currentItem();

    setEngineChorus(pEngine, iChorusNr, fChorusLevel, fChorusSpeed, fChorusDepth, iChorusType);
    refreshChorus();

    m_iChorusUpdated--;
    m_iChorusChanged = 0;
}


// Refresh gain panel controls.
void qsynthMainForm::refreshGain (void)
{
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    float fGain = ::fluid_synth_get_gain(pEngine->pSynth);

    GainDial->setValue(qsynth_iscale_clip(QSYNTH_GAIN_SCALE, fGain));
}


// Refresh reverb panel controls.
void qsynthMainForm::refreshReverb (void)
{
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    double fReverbRoom  = ::fluid_synth_get_reverb_roomsize(pEngine->pSynth);
    double fReverbDamp  = ::fluid_synth_get_reverb_damp(pEngine->pSynth);
    double fReverbWidth = ::fluid_synth_get_reverb_width(pEngine->pSynth);
    double fReverbLevel = ::fluid_synth_get_reverb_level(pEngine->pSynth);

    ReverbRoomDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, fReverbRoom));
    ReverbDampDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, fReverbDamp));
    ReverbWidthDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, fReverbWidth));
    ReverbLevelDial->setValue(qsynth_iscale_clip(QSYNTH_REVERB_SCALE, fReverbLevel));
}


// Refresh chorus panel controls.
void qsynthMainForm::refreshChorus (void)
{
    qsynthEngine *pEngine = currentEngine();
    if (pEngine == NULL)
        return;
    if (pEngine->pSynth == NULL)
        return;

    int    iChorusNr    = ::fluid_synth_get_chorus_nr(pEngine->pSynth);
    double fChorusLevel = ::fluid_synth_get_chorus_level(pEngine->pSynth);
    double fChorusSpeed = ::fluid_synth_get_chorus_speed_Hz(pEngine->pSynth);
    double fChorusDepth = ::fluid_synth_get_chorus_depth_ms(pEngine->pSynth);
    int    iChorusType  = ::fluid_synth_get_chorus_type(pEngine->pSynth);

    ChorusNrDial->setValue(iChorusNr);
    ChorusLevelDial->setValue(qsynth_iscale_clip(QSYNTH_CHORUS_SCALE, fChorusLevel));
    ChorusSpeedDial->setValue(qsynth_iscale_clip(QSYNTH_CHORUS_SCALE, fChorusSpeed));
    ChorusDepthDial->setValue(qsynth_iscale_clip(QSYNTH_CHORUS_SCALE, fChorusDepth));
    ChorusTypeComboBox->setCurrentItem(iChorusType);
}


// end of qsynthMainForm.ui.h
