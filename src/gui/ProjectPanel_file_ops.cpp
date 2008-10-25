/*

                          Firewall Builder

                 Copyright (C) 2008 NetCitadel, LLC

  Author:  alek@codeminders.com
           refactoring and bugfixes: vadim@fwbuilder.org

  $Id: ProjectPanel.cpp 417 2008-07-26 06:55:44Z vadim $

  This program is free software which we release under the GNU General Public
  License. You may redistribute and/or modify this program under the terms
  of that license as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  To get a copy of the GNU General Public License, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

*/

#include "../../config.h"
#include "global.h"
#include "utils.h"
#include "utils_no_qt.h"

#include <fwbuilder/RuleSet.h>
#include <fwbuilder/Policy.h>
#include <fwbuilder/NAT.h>
#include <fwbuilder/Routing.h>
#include <fwbuilder/Rule.h>
#include <fwbuilder/Interface.h>
#include <fwbuilder/Library.h>

#include "ProjectPanel.h"

#include <ui_rcsfilesavedialog_q.h>

#include "FWWindow.h"
#include "RCS.h"
#include "filePropDialog.h"
#include "FWBSettings.h"
#include "RCSFilePreview.h"
#include "FindObjectWidget.h"
#include "FWObjectClipboard.h"
#include "upgradePredicate.h"
#include "ObjConflictResolutionDialog.h"
#include "LibExportDialog.h"
#include "longTextDialog.h"

//#include "listOfLibraries.h"

#include <QMdiSubWindow>
#include <QMdiArea>
#include <QStatusBar>
#include <QTextStream>
#include <QFile>
#include <QTimer>

#include <list>
#include <algorithm>

#include <errno.h>

#include <libxml/tree.h>

#define LONG_ERROR_CUTOFF 1024

using namespace Ui;
using namespace libfwbuilder;
using namespace std;

bool ProjectPanel::saveIfModified()
{
    if (db() && db()->isDirty() && rcs && !rcs->getFileName().isEmpty())
    {
        QString message = "Some objects have been modified but not saved.\n";
        message += "Do you want to save ";
        message += rcs->getFileName();
        message += " changes now ?"; 
        switch (QMessageBox::information(this, "Firewall Builder",
            message,
            tr("&Save"), tr("&Discard"), tr("&Cancel"),
            0,       // Enter = button 0
            2 ) ) {   // Escape == button 2

        case 0:
            save();
            break;
        case 1:  // discard
            db()->setDirty(false);
            break;
        case 2:  // cancel
            return(false);
        }
    }
    return true;
}


QString ProjectPanel::chooseNewFileName(const QString &fname,
                                        const QString &title)
{
    QString destdir = getDestDir(fname);

    // Note that QFileDialog::getSaveFileName asks for confirmation
    // if the file already exists.
    QString fn = QFileDialog::getSaveFileName( this, title, destdir,
       tr( "FWB Files (*.fwb);;All Files (*)" ) );

    return fn;
}


void ProjectPanel::fileProp()
{
    if (rcs!=NULL)
    {
        filePropDialog fpd(this,rcs);
        fpd.setPrinter(mainW->getPrinter());
        fpd.exec();
    }
}

bool ProjectPanel::fileNew()
{
    if (fwbdebug) qDebug("ProjectPanel::fileNew()");

    QString nfn = chooseNewFileName(
        st->getWDir(), tr("Choose name and location for the new file"));

    if ( !nfn.isEmpty() )
    {
        //if (!saveIfModified() || !checkin(true)) return;
        if (!systemFile && rcs!=NULL) 
            fileClose();       // fileClose calls load(this)
        else  
            loadStandardObjects();

        visibleFirewall = NULL;
        setFileName(nfn);
        save();
        setupAutoSave();
    }

    if (fwbdebug)
        qDebug("ProjectPanel::fileNew()  rcs=%p  rcs->getFileName()='%s'",
               rcs, rcs->getFileName().toAscii().constData());

    return (rcs!=NULL);
}

bool ProjectPanel::fileOpen()
{
/*
 * there appears to be a bug on Mac OS X that causes main window to
 * switch to a different MDI subwindow if user opens "File Open"
 * dialog and cancels operation by clicking "Cancel". This does not
 * happen on other platforms so this must be Mac only thing. To work
 * around the issue we save pointer to the MDI subwindow that was
 * active before activation of the "OpenFile" dialog and restore it if
 * user cancelled operation.
 *
 * This only happens in fileOpen(). fileImport() uses the same
 * QFileDialog dialog but does not trigger this effect. Perhaps this
 * happens because we call fileOpen from newly created but still
 * hidden ProjectPanel object. This is the only difference I can think
 * of.
 */
    QMdiSubWindow *current_active_window=mainW->getMdiArea()->activeSubWindow();

    QString dir;
    dir=st->getWDir();
    if (dir.isEmpty())  dir=st->getOpenFileDir();
    if (dir.isEmpty())  dir=userDataDir.c_str();

    QString fileName = QFileDialog::getOpenFileName(
        mainW,
        tr("Open File"),
        dir,
        "FWB files (*.fwb *.fwl *.xml);;All Files (*)");
    
    if (fileName.isEmpty())
    {
        mainW->getMdiArea()->setActiveSubWindow(current_active_window);
        return false;
    }

    return loadFile(fileName);
}

bool ProjectPanel::loadFile(const QString &fileName)
{
    if (fwbdebug) qDebug("ProjectPanel::loadFile  fileName=%s",
                         fileName.toLocal8Bit().constData());

    RCSFilePreview  fp(this);
    bool hasRCS = fp.showFileRLog(fileName);

    if ( (!hasRCS) || (fp.exec() == QDialog::Accepted) )
    {
        if (!saveIfModified() || !checkin(true)) return false;
        if (!systemFile && rcs!=NULL) fileClose();

        //try to get simple rcs instance from RCS preview
        RCS *new_rcs = fp.getSelectedRev();

        //if preview cannot give RCS,
        //get a new RCS from file dialog
        if (new_rcs==NULL) new_rcs = new RCS(fileName);
        if (new_rcs==NULL) return false;

        try
        {
            new_rcs->co();
        } catch (FWException &ex)
        {
            return false;
        }

        loadFromRCS(new_rcs);

        if (new_rcs->isTemp())
            unlink(new_rcs->getFileName().toLocal8Bit().constData());

        return true;
    }
    return false;
}

void ProjectPanel::fileClose()
{
    if (fwbdebug) qDebug("ProjectPanel::fileClose(): start");

    closing = true ;

    findObjectWidget->init();
    if (isEditorVisible()) hideEditor();

    if (!saveIfModified() || !checkin(true))  return;

    if (rcs) delete rcs;
    rcs=NULL;

    if (fwbdebug) qDebug("ProjectPanel::fileClose(): clearing widgets");

    FWObjectClipboard::obj_clipboard->clear();

    mdiWindow->close();

    firewalls.clear();
    visibleFirewall = NULL;
    visibleRuleSet = NULL;
    clearFirewallTabs();
    clearObjects();

    if (fwbdebug) qDebug("ProjectPanel::fileClose(): done");
}

void ProjectPanel::fileSave()
{
    QStatusBar *sb = mainW->statusBar();
    sb->showMessage( tr("Saving data to file...") );
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents,100);
    save();
    sb->clearMessage();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents,100);
}

void ProjectPanel::fileSaveAs()
{
    if (isEditorVisible()) hideEditor();

/* we need to save data into the current file before we save it into a
 * new file, provided we do have current file
    if (!systemFile && rcs &&
        !rcs->isRO() && !rcs->isTemp() && !rcs->getFileName().isEmpty() &&
        (!saveIfModified() || !checkin(true))
    ) return;

 */


/* need to close the file without asking and saving, then reopen it again */

    QString oldFileName = rcs->getFileName();

    QString nfn = chooseNewFileName(
        oldFileName, tr("Choose name and location for the file"));

    if (!nfn.isEmpty())
    {
        db()->setDirty(false);  // so it wont ask if user wants to save
        rcs->abandon();

        if (rcs!=NULL) delete rcs;

        rcs = new RCS("");

        setFileName(nfn);

        save();
    }
}

void ProjectPanel::fileExit()
{
    if (saveIfModified() && checkin(true))
    {
        saveState();
        if (rcs) delete rcs;
        qApp->quit();
    }
}

void ProjectPanel::fileCommit()
{
    save();
    if (!checkin(true))  return;
    rcs->co();
}

/*
 * discard changes done to the file and check out clean copy of the
 * head revision from RCS
 */
void ProjectPanel::fileDiscard()
{
    if (QMessageBox::warning(this, "Firewall Builder",
      tr("This operation discards all changes that have been saved\n"
         "into the file so far, closes it and replaces it with a clean\n"
         "copy of its head revision from RCS.\n"
         "\n"
         "All changes will be lost if you do this.\n\n"),
      tr("&Discard changes"),
      tr("&Cancel"), QString::null,
      1 )==0 )
    {
        /* need to close the file without asking and saving, then
         * reopen it again
         */

        QString fname = rcs->getFileName();

        db()->setDirty(false);  // so it wont ask if user wants to save
        rcs->abandon();

        /* do everything fileClose() does except do not close mdiWindow
         * because we'll need it again to reopen the file into
         */
        findObjectWidget->init();
        if (isEditorVisible()) hideEditor();

        if (rcs) delete rcs;
        rcs=NULL;

        FWObjectClipboard::obj_clipboard->clear();

        firewalls.clear();
        visibleFirewall = NULL;
        visibleRuleSet = NULL;
        clearFirewallTabs();
        clearObjects();

        /* loadFile calls fileClose, but only if file is currently
         * open, which it isn't because we reset rcs to NULL
         */
        loadFile(fname);
    }
}

void ProjectPanel::fileAddToRCS()
{
    if (!saveIfModified()) return;
    if (rcs && rcs->isCheckedOut()) return;

    try
    {
        if (!rcs->isInRCS() && !rcs->isRO())
        {
            rcs->add();
            rcs->co();
            QMessageBox::information(
                this,"Firewall Builder",
                tr("File %1 has been added to RCS.").arg(rcs->getFileName()),
                tr("&Continue"), QString::null,QString::null,
                0, 1 );
        }
    }
    catch (FWException &ex)
    {
        QMessageBox::critical(
            this,"Firewall Builder",
            tr("Error adding file to RCS:\n%1").arg(ex.toString().c_str()),
            tr("&Continue"), QString::null,QString::null,
            0, 1 );
    }

    setWindowTitle(getPageTitle());
}

void ProjectPanel::fileImport()
{
    resetFD();

    QString fname = QFileDialog::getOpenFileName(
        mainW,
        tr("Choose a file to import"),
        st->getWDir(),
        "FWB library files (*.fwl);;FWB Files (*.fwb);;All Files (*)");

    if (fname.isEmpty()) return;   // Cancel  - keep working with old file

    loadLibrary( fname.toLocal8Bit().constData() );

    loadObjects();
}


void ProjectPanel::fileCompare()
{
    resetFD();

    QMessageBox initial_question( QMessageBox::Information, "Firewall Builder",
                    tr("This operation inspects two data files (either .fwb or .fwl) and finds conflicting objects. Conflicting objects have the same internal ID but different attributes. Two data files can not be merged, or one imported into another, if they contain such objects. This operation also helps identify changes made to objects in two copies of the same data file.<br><br>This operation does not find objects present in one file but not in the other, such objects present no problem for merge or import operations.<br><br>This operation works with two external files, neither of which needs to be opened in the program. Currently opened data file is not affected by this operation and objects in the tree do not change.<br><br>Do you want to proceed ?"),
                    QMessageBox::Yes | QMessageBox::No);

    initial_question.setTextFormat( Qt::RichText );
    if (initial_question.exec() != QMessageBox::Yes) return;


    QString fname1 = QFileDialog::getOpenFileName(
        mainW,
        tr("Choose the first file"),
        st->getWDir(),
        "FWB files (*.fwb);;FWB Library Files (*.fwl);;All Files (*)");

    if (fname1.isEmpty()) return;   // Cancel

    QString fname2 = QFileDialog::getOpenFileName(
        mainW,
        tr("Choose the second file"),
        st->getWDir(),
        "FWB files (*.fwb);;FWB Library Files (*.fwl);;All Files (*)");

    if (fname2.isEmpty()) return;   // Cancel

    MessageBoxUpgradePredicate upgrade_predicate(mainW);

    FWObjectDatabase *db1;
    FWObjectDatabase *db2;
    FWObject *dobj;

    try
    {
        db1 = new FWObjectDatabase();
        db1->load(fname1.toLocal8Bit().constData(),
                  &upgrade_predicate,  librespath);

        dobj = db1->findInIndex(FWObjectDatabase::DELETED_OBJECTS_ID);
        if (dobj) db1->remove(dobj, false);
    } catch(FWException &ex)
    {
        QMessageBox::critical(
            this,"Firewall Builder",
            tr("Error loading file %1:\n%2").
                 arg(fname1).arg(ex.toString().c_str()),
            tr("&Continue"), QString::null,QString::null,
            0, 1 );
        return;
    }

    try
    {
        db2 = new FWObjectDatabase();
        db2->load(fname2.toLocal8Bit().constData(),
                  &upgrade_predicate,  librespath);

        dobj = db2->findInIndex(FWObjectDatabase::DELETED_OBJECTS_ID);
        if (dobj) db2->remove(dobj, false);
    } catch(FWException &ex)
    {
        QMessageBox::critical(
            this,"Firewall Builder",
            tr("Error loading file %1:\n%2").
                 arg(fname2).arg(ex.toString().c_str()),
            tr("&Continue"), QString::null,QString::null,
            0, 1 );
        return;
    }

    try
    {
        // CompareObjectsDialog is just like ObjConflictResolutionDialog
        // except it always returns 'accepted' and keeps record
        // of all object differences so we can print report in the end

        CompareObjectsDialog cod(this);
        db1->merge(db2, &cod);
        list<QString> report = cod.getReport();

        delete db1;
        delete db2;

        ostringstream str;
        str << cod.getNumberOfConflicts();

        QMessageBox mb( QMessageBox::Information, "Firewall Builder",
                        tr("Total number of conflicting objects: %1.\nDo you want to generate report?").arg(str.str().c_str()),
                        QMessageBox::Yes | QMessageBox::No);

        if (mb.exec() == QMessageBox::Yes)
        {
            // save report to a file

            QString destdir = getDestDir(fname1);

            QString fn = QFileDialog::getSaveFileName( this,
                           tr("Choose name and location for the report file"),
                           destdir,
                           tr( "TXT Files (*.txt);;All Files (*)" ));

            if (fwbdebug)
                qDebug( QString("Saving report to %1").arg(fn).toAscii().constData() );

            if (fn.isEmpty() ) return ;   // Cancel

            if (!fn.endsWith(".txt"))
            {
                fn+=".txt";
            }

            QFile report_file(fn);
            if (report_file.open(QIODevice::WriteOnly))
            {
                QTextStream report_stream(&report_file);
                for (list<QString>::iterator i=report.begin(); i!=report.end(); ++i)
                {
                    report_stream << *i;
                }
                report_file.close();
            } else
            {
                QMessageBox::critical(
                    this,"Firewall Builder",
                    tr("Can not open report file for writing. File '%1'").arg(fn),
                    tr("&Continue"), QString::null,QString::null,
                    0, 1 );
            }

        }

    } catch(FWException &ex)
    {
        QMessageBox::critical(
            this,"Firewall Builder",
            tr("Unexpected error comparing files %1 and %2:\n%3").
                 arg(fname1).arg(fname2).arg(ex.toString().c_str()),
            tr("&Continue"), QString::null,QString::null,
            0, 1 );
    }

}

void ProjectPanel::fileExport()
{
    LibExportDialog ed;
    list<FWObject*>  selectedLibs;
    map<int,FWObject*>::iterator i;
    QString path="";
    int lib_idx = -1;
    do
    {
        if (ed.exec()!=QDialog::Accepted) return;

        QList<QListWidgetItem*> selitems = ed.m_dialog->libs->selectedItems();

        for (i=ed.mapOfLibs.begin(); i!=ed.mapOfLibs.end(); i++)
            if (selitems.contains(ed.m_dialog->libs->item(i->first)))
                selectedLibs.push_back(i->second);

        lib_idx=ed.m_dialog->libs->currentRow ();

        if (lib_idx<0 || selectedLibs.size()==0)
        {
            QMessageBox::critical(
                this,"Firewall Builder",
                tr("Please select a library you want to export."),
                "&Continue", QString::null,QString::null,
                0, 1 );

            return;
        }
    } while (!exportLibraryTest(selectedLibs));

    FWObject *selLib = ed.mapOfLibs[ lib_idx ];
    path=st->getWDir()+QString::fromUtf8(selLib->getName().c_str())+".fwl";

    resetFD();
    QString fname;
        fname = QFileDialog::getSaveFileName(
            this,
            "Choose a filename to save under",
            path,
            "Firewall Builder 2 files (*.fwl)");

    if (fname.isEmpty()) return;
    if (QFile::exists(fname) &&
         QMessageBox::warning(
             this,"Firewall Builder",
             tr("The file %1 already exists.\nDo you want to overwrite it ?")
             .arg(fname),
             tr("&Yes"), tr("&No"), QString::null,
             0, 1 )==1 ) return;
    exportLibraryTo(fname,selectedLibs,ed.m_dialog->exportRO->isChecked());
}

bool ProjectPanel::exportLibraryTest(list<FWObject*> &selectedLibs)
{
/* VERY IMPORTANT: External library file must be self-contained,
 * otherwise it can not be exported.
 *
 * check if selected libraries have references to objects in other
 * libraries (not exported to the same file). Exporting such libraries
 * pulls in other ones because of these references. This is confusing
 * because it means we end up with multiple copies of such objects (in
 * exported library file and in user's data file). When user imports
 * this library and opens their file, it is impossible to say which
 * library an object belongs to.
 *
 * This is prohibited. We check if exported set of libraries has such
 * references and refuse to export it. The user is supposed to clean
 * it up by either moving objects into the library they are trying to
 * export, or by rearranging objects. The only exception for this is
 * library "Standard", which is assumed to be always present so we can
 * have references to objects in it.
 */
    QApplication::setOverrideCursor( QCursor( Qt::WaitCursor) );

    list<FWReference*> externalRefs;
    for (list<FWObject*>::iterator i=selectedLibs.begin(); i!=selectedLibs.end(); ++i)
        findExternalRefs( *i, *i, externalRefs);

    QApplication::restoreOverrideCursor();

    if (fwbdebug) qDebug("LibExportDialog::accept  externalRefs.size()=%d",
                         int(externalRefs.size()) );

/*
 * if externalRefs.size()!=0, then there were some references pointing
 * outside of the libraries we export. Some of these references may
 * point at other libraries we export, lets find these.
 */
    list<FWReference*> externalRefs2;
    for (list<FWReference*>::iterator i=externalRefs.begin(); i!=externalRefs.end(); ++i)
    {
        FWObject *tgt    = (*i)->getPointer();
        FWObject *tgtlib = tgt->getLibrary();

        if (std::find(selectedLibs.begin(),selectedLibs.end(),tgtlib)!=selectedLibs.end()) continue;
        externalRefs2.push_back(*i);
    }

    if (externalRefs2.size()!=0)
    {
        QString objlist = "";
        QString s       = "";

        for (list<FWReference*>::iterator i=externalRefs2.begin(); i!=externalRefs2.end(); ++i)
        {
            FWReference *robj   = *i;
            FWObject *selLib = robj->getLibrary();
            FWObject *pp     = robj->getParent();
            FWObject *tgt    = robj->getPointer();
            FWObject *tgtlib = tgt->getLibrary();

            if (fwbdebug)
            {
                qDebug("LibExportDialog::accept  tgt: %s pp_type: %s lib: %s",
                       tgt->getName().c_str(),
                       pp->getTypeName().c_str(),
                       tgtlib->getName().c_str());
            }

            if (std::find(selectedLibs.begin(),selectedLibs.end(),tgtlib)!=selectedLibs.end()) continue;

            if (RuleElement::cast(pp)!=NULL)
            {
                FWObject *fw       = pp;
                FWObject *rule     = pp;
                FWObject *ruleset  = pp;
                FWObject *iface    = pp;

                while (rule!=NULL && Rule::cast(rule)==NULL)
                    rule=rule->getParent();
                while (ruleset!=NULL && RuleSet::cast(ruleset)==NULL)
                    ruleset=ruleset->getParent();
                while (iface!=NULL && Interface::cast(iface)==NULL)
                    iface=iface->getParent();
                while (fw!=NULL && Firewall::cast(fw)==NULL)
                    fw=fw->getParent();

                QString rsname;
                if (Policy::cast(ruleset)!=NULL)
                {
                    s =
   QObject::tr("Library %1: Firewall '%2' (global policy rule #%3) uses object '%4' from library '%5'")
                        .arg(selLib->getName().c_str())
                        .arg(fw->getName().c_str())
                        .arg(Rule::cast(rule)->getPosition())
                        .arg(tgt->getName().c_str())
                        .arg(tgtlib->getName().c_str());
                }
                if (NAT::cast(ruleset)!=NULL)
                {
                    s =
   QObject::tr("Library %1: Firewall '%2' (NAT rule #%3) uses object '%4' from library '%5'")
                        .arg(selLib->getName().c_str())
                        .arg(fw->getName().c_str())
                        .arg(Rule::cast(rule)->getPosition())
                        .arg(tgt->getName().c_str())
                        .arg(tgtlib->getName().c_str());
                }
            } else
            {
                    s =
   QObject::tr("Library %1: Group '%2' uses object '%3' from library '%4'")
                        .arg(selLib->getName().c_str())
                        .arg(pp->getName().c_str())
                        .arg(tgt->getName().c_str())
                        .arg(tgtlib->getName().c_str());
            }
            s = s + "\n";

            if (fwbdebug) qDebug(s.toAscii().constData());

            objlist = objlist + s;
        }

        longTextDialog ltd( this,

            tr("A library that you are trying to export contains references\n"
               "to objects in the other libraries and can not be exported.\n"
               "The following objects need to be moved outside of it or\n"
               "objects that they refer to moved in it:"),
                            objlist );

        ltd.exec();
        return false;
    }
    return true;
}

void ProjectPanel::exportLibraryTo(QString fname,list<FWObject*> &selectedLibs, bool rof)
{
    QApplication::setOverrideCursor( QCursor( Qt::WaitCursor) );

    FWObjectDatabase *ndb = db()->exportSubtree( selectedLibs );

    QApplication::restoreOverrideCursor();

    if (rof)
    {
        for (list<FWObject*>::iterator i=selectedLibs.begin(); i!=selectedLibs.end(); ++i)
        {
            FWObject *nlib= ndb->findInIndex( (*i)->getId() );
            if (nlib && nlib->getId()!=FWObjectDatabase::DELETED_OBJECTS_ID)
                nlib->setReadOnly( true );
        }
    }

    try
    {
        xmlSetCompressMode(st->getCompression() ? 9 : 0);
        ndb->saveFile( fname.toLocal8Bit().constData() );
    }
    catch (FWException &ex)
    {
/* error saving the file. Since XMLTools does not return any useful
 * error message in the exception, let's check for obvious problems here
 */
        QString err;
        if (access( fname.toLocal8Bit().constData(), W_OK)!=0 && errno==EACCES)
            err=QObject::tr("File is read-only");

        QMessageBox::critical(
            this,"Firewall Builder",
            QObject::tr("Error saving file %1: %2")
            .arg(fname).arg(err),
            "&Continue", QString::null, QString::null,
            0, 1 );
    }
}

void ProjectPanel::setupAutoSave()
{
    if ( st->getBool("Environment/autoSaveFile") &&
         rcs!=NULL && rcs->getFileName()!="")
    {
        int p = st->getInt("Environment/autoSaveFilePeriod");
        autosaveTimer->start( p*1000*60 );
        connect( autosaveTimer, SIGNAL(timeout()), this, SLOT(fileSave()) );
    } else
        autosaveTimer->stop();
}

void ProjectPanel::findExternalRefs(FWObject *lib,
                                       FWObject *root,
                                       list<FWReference*> &extRefs)
{
    FWReference *ref=FWReference::cast(root);
    if (ref!=NULL)
    {
        FWObject *plib = ref->getPointer()->getLibrary();
        if ( plib->getId()!=FWObjectDatabase::STANDARD_LIB_ID &&
             plib->getId()!=FWObjectDatabase::DELETED_OBJECTS_ID  &&
             plib!=lib )
            extRefs.push_back(ref);
        return;
    } else
    {
        for (FWObject::iterator i=root->begin(); i!=root->end(); i++)
            findExternalRefs(lib, *i, extRefs);

    }
}

void ProjectPanel::loadLibrary(const string &libfpath)
{
    MessageBoxUpgradePredicate upgrade_predicate(mainW);

    try
    {
        FWObjectDatabase *ndb = new FWObjectDatabase();
        ndb->load(libfpath,  &upgrade_predicate,  librespath);

        FWObject *dobj = ndb->findInIndex(FWObjectDatabase::DELETED_OBJECTS_ID);
        if (dobj) ndb->remove(dobj, false);

        MergeConflictRes mcr(this);
        db()->merge(ndb, &mcr);
        delete ndb;
    } catch(FWException &ex)
    {
        QString error_txt = ex.toString().c_str();
        if (error_txt.length() > LONG_ERROR_CUTOFF) 
        {
            error_txt.truncate(LONG_ERROR_CUTOFF);
            error_txt += "\n\n" + tr("(Long error message was truncated)");
        }
        QMessageBox::critical(
            this,"Firewall Builder",
            tr("The program encountered error trying to load file %1.\n"
               "The file has not been loaded. Error:\n%2").
                 arg(libfpath.c_str()).arg(error_txt),
            tr("&Continue"), QString::null,QString::null,
            0, 1 );
    }
}

/*
 * Load standard library objects
 */
void ProjectPanel::loadStandardObjects()
{
    if (fwbdebug) qDebug("ProjectPanel::load(): start");

    QStatusBar *sb = mainW->statusBar();

    editingStandardLib = false;
    editingTemplateLib = false;

    MessageBoxUpgradePredicate upgrade_predicate(mainW);

    resetFD();

    try
    {
// need to drop read-only flag on the database before I load new objects

        objdb = new FWObjectDatabase();
        objdb->setReadOnly( false );

        sb->showMessage( tr("Loading system objects...") );
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents,100);

// always load system objects
        if (fwbdebug)
            qDebug("ProjectPanel::load(): sysfname = %s", sysfname.c_str());

        objdb->load( sysfname, &upgrade_predicate, librespath);
        objdb->setFileName("");

        if (fwbdebug) qDebug("ProjectPanel::load(): create User library");

        FWObject *userLib = createNewLibrary( objdb );
        userLib->setName("User");
        userLib->setStr("color","#d2ffd0");

        objdb->setDirty(false);
        objdb->setFileName("");

        createRCS("");

        setWindowTitle(getPageTitle());

        loadObjects();
        setupAutoSave();

        time_t last_modified = objdb->getTimeLastModified();
        if (fwbdebug)
            qDebug("ProjectPanel::load(): done  last_modified=%s dirty=%d",
                   ctime(&last_modified), objdb->isDirty());
    } catch(FWException &ex)
    {
        QMessageBox::critical(
            this,"Firewall Builder",
            tr("Error loading file:\n%1").arg(ex.toString().c_str()),
            tr("&Continue"), QString::null,QString::null,
            0, 1 );
    }

    sb->clearMessage();
}

void ProjectPanel::loadFromRCS(RCS *_rcs)
{
    QStatusBar *sb = mainW->statusBar();

    resetFD();

    editingStandardLib = false;
    editingTemplateLib = false;

    bool forceSave=false; // use this flag to force 'save' operation if file should be renamed

    MessageBoxUpgradePredicate upgrade_predicate(mainW);

    assert(_rcs!=NULL);

    rcs = _rcs;
    try
    {
        /* load the data file */
        systemFile = false;

        objdb = new FWObjectDatabase();

// need to drop read-only flag on the database before I load new objects
        objdb->setReadOnly( false );

// always loading system objects
        sb->showMessage( tr("Loading system objects...") );
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        objdb->load( sysfname, &upgrade_predicate, librespath);
        objdb->setFileName("");

// objects from a data file are in database ndb

        sb->showMessage( tr("Reading and parsing data file...") );
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        FWObjectDatabase *ndb = new FWObjectDatabase();
        ndb->load(rcs->getFileName().toLocal8Bit().constData(),
                  &upgrade_predicate,librespath);
        time_t   oldtimestamp = ndb->getTimeLastModified();

        sb->clearMessage();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

/* loadingLib is true if user wants to open a library or master library file */
        bool loadingLib         = editingLibrary();

        if (fwbdebug)
        {
            list<FWObject*> ll = ndb->getByType(Library::TYPENAME);
            for (FWObject::iterator i=ll.begin(); i!=ll.end(); i++)
            {
                qDebug("* Found library %s %s in the data file",
                       FWObjectDatabase::getStringId((*i)->getId()).c_str(),
                       (*i)->getName().c_str() );
            }
        }

/* if user opens library file, clear read-only flag so they can edit it */
        if (loadingLib)
        {
            list<FWObject*> ll = ndb->getByType(Library::TYPENAME);
            for (FWObject::iterator i=ll.begin(); i!=ll.end(); i++)
            {
                if ((*i)->getId()==FWObjectDatabase::STANDARD_LIB_ID)
                    editingStandardLib=true;
                if ((*i)->getId()==FWObjectDatabase::TEMPLATE_LIB_ID)
                    editingTemplateLib=true;
                (*i)->setReadOnly( false );
            }
        }

        sb->showMessage( tr("Merging with system objects...") );
        QCoreApplication::processEvents(
            QEventLoop::ExcludeUserInputEvents, 100);

        MergeConflictRes mcr(mainW);
        objdb->merge(ndb, &mcr);

        delete ndb;

        objdb->setFileName(rcs->getFileName().toLocal8Bit().constData());
        objdb->resetTimeLastModified(oldtimestamp);
        objdb->setDirty(false);

        sb->clearMessage();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents,100);

        if (fwbdebug)
        {
            qDebug("* Merge is done");
            list<FWObject*> ll = db()->getByType(Library::TYPENAME);
            for (FWObject::iterator i=ll.begin(); i!=ll.end(); i++)
            {
                qDebug("* Library %s %s in the data file",
                       FWObjectDatabase::getStringId((*i)->getId()).c_str(),
                       (*i)->getName().c_str() );
            }
        }


        /* this is a hack: 'Standard' library should be read-only. I
         * have too many files I already converted to the new API/DTD
         * and I am too lazy to convert them again, so I patch it up
         * here.
         *
         * However, if I am editing standard library, it should not be
         * read-only.
         */
        FWObject *slib = objdb->findInIndex(FWObjectDatabase::STANDARD_LIB_ID);
        if (slib!=NULL )
        {
            if (fwbdebug)
                qDebug("standard library read-only status: %d, "
                       "editingStandardLib: %d",
                       slib->isReadOnly(), editingStandardLib);

            slib->setReadOnly(! editingStandardLib);
        }

        /* if the file name has an old extension .xml, change it to .fwb and
         * warn the user
         */
        QString   fn = rcs->getFileName();
        QFileInfo ofinfo(fn);

        if ( ofinfo.suffix()=="xml")
        {
            if (fwbdebug)
            {
                qDebug("Need to rename file:  %s",
                       fn.toAscii().constData());
                qDebug("             dirPath: %s",
                       ofinfo.dir().absolutePath().toAscii().constData());
                qDebug("            filePath: %s",
                       ofinfo.absoluteFilePath().toAscii().constData());
            }
            QString nfn = ofinfo.dir().absolutePath()
                + "/" + ofinfo.completeBaseName() + ".fwb";

            bool needToRename = true;

            /* need these dances with symlinks to fix bug #1008956:
             * "Existing .fwb file gets overwritten if has wrong
             * extension"
             */
            QFileInfo nfinfo(nfn);
            if (nfinfo.exists() && ofinfo.isSymLink() && ofinfo.readLink()==nfn)
            {
                // .xml file is a symlink pointing at .fwb file
                // no need to rename
                needToRename = false;
            }

            if (needToRename)
            {
                if (nfinfo.exists())
                {
                    /* .fwb file exists but .xml is not a symlink
                     * .fwb is a separate file with the same name.
                     *
                     * tell the user we need to rename old file but
                     * the new file exists, then ask them to choose a
                     * new name. If the user chooses the same name and
                     * agrees to overwrite the file, just use this
                     * name. If the user hits cancel, tell them they
                     * need to choose a new name and open "file save"
                     * dialog again.
                     *
                     * Show the first dialog only once. If user hits
                     * Cancel, they see shorted version of the dialog
                     * and will be presented with "save file" dialog
                     * again.
                     */
                    QMessageBox::warning(
                        this,"Firewall Builder",
                        tr("Firewall Builder 2 uses file extension '.fwb' and\n"
                           "needs to rename old data file '%1' to '%2',\n"
                           "but file '%3' already exists.\n"
                           "Choose a different name for the new file.")
                        .arg(fn).arg(nfn).arg(nfn),
                        tr("&Continue"), QString::null,QString::null,
                        0, 1 );

                    nfn = chooseNewFileName(
                        fn, tr("Choose name and location for the new file"));
                    if (nfn.isEmpty())
                    {
                        QString oldFileName = ofinfo.absoluteFilePath()
                            + ".bak";
                        rename(oldFileName.toLocal8Bit().constData(),
                               fn.toLocal8Bit().constData());

                        QMessageBox::warning(
                            this,"Firewall Builder",
                            tr("Load operation cancelled and data file reverted"
                               "to original version."),
                            tr("&Continue"), QString::null,QString::null,
                            0, 1 );

                        loadStandardObjects();
                        return;
                    }
                    nfinfo.setFile(nfn);
                }

                rename(fn.toLocal8Bit().constData(),
                       nfn.toLocal8Bit().constData());

                QMessageBox::warning(
                this,"Firewall Builder",
                tr("Firewall Builder 2 uses file extension '.fwb'. Your data"
                   "file '%1' \nhas been renamed '%2'")
                .arg(fn).arg(nfn),
                tr("&Continue"), QString::null,QString::null,
                0, 1 );
            }

            fn = nfn;
        }

        rcs->setFileName(fn);
        db()->setFileName(fn.toLocal8Bit().constData());

        setWindowTitle(getPageTitle());

        mainW->disableActions(m_panel->ruleSets->count()!=0);

        time_t last_modified = db()->getTimeLastModified();
        if (fwbdebug)
            qDebug("ProjectPanel::load(): load complete dirty=%d "
                   "last_modified=%s",
                   db()->isDirty(), ctime(&last_modified));

    } catch(FWException &ex)
    {
        QString trans = ex.getProperties()["failed_transformation"].c_str();
        QString elem  = ex.getProperties()["failed_element"].c_str();

        if(!trans.isEmpty() || !elem.isEmpty())
        {
            QString msg = tr("Exception: %1").arg(ex.toString().c_str());
            if (!trans.isEmpty())
            {
                trans.truncate(LONG_ERROR_CUTOFF);
                msg+="\n"+tr("Failed transformation : %1").arg(trans);
            }
            if (!elem.isEmpty())
            {
                elem.truncate(LONG_ERROR_CUTOFF);
                msg+="\n"+tr("XML element : %1").arg(elem);
            }
            QMessageBox::critical(
                this,"Firewall Builder",
                tr("The program encountered error trying to load data file.\n"
                   "The file has not been loaded. Error:\n%1").arg(msg),
                tr("&Continue"), QString::null,QString::null,
                0, 1 );
        } else
        {
            QString error_txt = ex.toString().c_str();
            if (error_txt.length() > LONG_ERROR_CUTOFF) 
            {
                error_txt.truncate(LONG_ERROR_CUTOFF);
                error_txt += "\n\n" + tr("(Long error message was truncated)");
            }

            QMessageBox::critical(
                this,"Firewall Builder",
                tr("The program encountered error trying to load data file.\n"
                   "The file has not been loaded. Error:\n%1").arg(
                       error_txt),
                tr("&Continue"), QString::null,QString::null,
                0, 1 );
        }
        // load standard objects so the window does not remain empty
        loadStandardObjects();
        return;
    }

    db()->setReadOnly( rcs->isRO() || rcs->isTemp() );

// clear dirty flag for all objects, recursively
    if (!forceSave)  db()->setDirty(false);

    sb->showMessage( tr("Building object tree...") );
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100);

    loadObjects();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100);

    sb->showMessage( tr("Indexing...") );
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100);
    db()->reIndex();

    sb->clearMessage();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 100);

    setupAutoSave();

    time_t last_modified = db()->getTimeLastModified();
    if (fwbdebug)
        qDebug("ProjectPanel::load(): all done: "
               "dirty=%d last_modified=%s",
               db()->isDirty(), ctime(&last_modified));
}

bool ProjectPanel::checkin(bool unlock)
{
/* doing checkin only if we did checkout so rcs!=NULL */
    QString rlog="";

    if (systemFile || rcs==NULL || !rcs->isCheckedOut() || rcs->isTemp())
        return true;

    if (rcs->isDiff())
    {
        // if the file hasn't changed, do not need to ask for the comment
        if ( ! st->getRCSLogState())
        {
            RCSFileSaveDialog_q fsd;
            QDialog *fsd_dialog = new QDialog(this);
            fsd.setupUi(fsd_dialog);
            fsd.checkinDialogTitle->setText(
                QString("<b>") +
                tr("Checking file %1 in RCS").arg(rcs->getFileName()) +
                QString("</b>")
            );
            if ( fsd_dialog->exec()== QDialog::Rejected )
            {
                delete fsd_dialog;
                return false;
            }

            bool    empty_rcslog = fsd.nolog->isChecked();
            if (empty_rcslog)
            {
                rlog = "";
                st->setRCSLogState(true);
            } else
                rlog = fsd.rcslog->toPlainText();

            delete fsd_dialog;
        }
    }


/***********************************************************************/
    try
    {
        if (fwbdebug)  qDebug("about to check the file in");
        rcs->ci(rlog,unlock);
        if (fwbdebug)  qDebug("done");
    }
    catch (FWException &ex)
    {
        QMessageBox::critical(
            this,"Firewall Builder",
            tr("Error checking in file %1:\n%2")
            .arg(rcs->getFileName()).arg(ex.toString().c_str()),
            tr("&Continue"), QString::null, QString::null,
            0, 1 );
    }
/***********************************************************************/
    return true;
}

void ProjectPanel::save()
{
    if (fwbdebug)
        qDebug("ProjectPanel::save:  rcs=%p  rcs->isRO=%d  "
               "rcs->isTemp=%d rcs->getFileName=%s",
               rcs,
               rcs->isRO(),
               rcs->isTemp(),
               rcs->getFileName().toLocal8Bit().constData());

    if (!rcs->isRO() && !rcs->isTemp())
    {
        try
        {
            if (rcs->getFileName().isEmpty())
                fileSaveAs();  // eventually calls this method again
            else
            {
/* editingLibfile is true if user edits a library or master library file */
                bool editingLibfile=editingLibrary();

                if (st->getDontSaveStdLib())  // this is now default
                {
                    list<FWObject*> userLibs;
                    list<FWObject*> ll = db()->getByType(Library::TYPENAME);
                    for (FWObject::iterator i=ll.begin(); i!=ll.end(); i++)
                    {
                        if (fwbdebug) qDebug("ProjectPanel::save()  lib %s",
                                             (*i)->getName().c_str() );

/* skip standard and template libraries unless we edit them */
                        int id = (*i)->getId();
                        if (id==FWObjectDatabase::STANDARD_LIB_ID && 
                            !editingStandardLib) continue;
                        if (id==FWObjectDatabase::TEMPLATE_LIB_ID &&
                            !editingTemplateLib) continue;

                        if (fwbdebug) qDebug("                   add");
                        userLibs.push_back( *i );
                    }

                    QApplication::setOverrideCursor(QCursor( Qt::WaitCursor));

                    FWObjectDatabase *ndb = db()->exportSubtree(userLibs);

                    if (editingLibfile)
                    {
/* exported libraries are always read-only */
                        list<FWObject*> ll = ndb->getByType(Library::TYPENAME);
                        for (FWObject::iterator i=ll.begin(); i!=ll.end(); i++)
                        {
                            if ((*i)->getId()!=FWObjectDatabase::STANDARD_LIB_ID
                                &&
                                (*i)->getId()!=FWObjectDatabase::DELETED_OBJECTS_ID)
                                (*i)->setReadOnly( true );
                        }
                    }

                    ndb->resetTimeLastModified( db()->getTimeLastModified() );
                    xmlSetCompressMode(st->getCompression() ? 9 : 0);
                    ndb->saveFile(
                        rcs->getFileName().toLocal8Bit().constData());

                    delete ndb;

                    QApplication::restoreOverrideCursor();

                } else
                {
                    QApplication::setOverrideCursor(QCursor( Qt::WaitCursor));
                    xmlSetCompressMode(st->getCompression() ? 9 : 0);
                    db()->saveFile(
                        rcs->getFileName().toLocal8Bit().constData());
                    QApplication::restoreOverrideCursor();
                }
            }
            db()->setDirty(false);
        }
        catch (FWException &ex)
        {
            QApplication::restoreOverrideCursor();

/* error saving the file. Since XMLTools does not return any useful
 * error message in the exception, let's check for obvious problems here
 */
            QString err;
            if (access(
                    rcs->getFileName().toLocal8Bit().constData(), W_OK)!=0 &&
                errno==EACCES)  err=tr("File is read-only");
            else                err=ex.toString().c_str();

            QMessageBox::critical(
                this,"Firewall Builder",
                tr("Error saving file %1: %2")
                .arg(rcs->getFileName()).arg(err),
                tr("&Continue"), QString::null, QString::null,
                0, 1 );
        }
    }
}

