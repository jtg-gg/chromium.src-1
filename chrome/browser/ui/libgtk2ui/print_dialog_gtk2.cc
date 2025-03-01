// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/libgtk2ui/print_dialog_gtk2.h"

#include <gtk/gtkunixprint.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "base/bind.h"
#include "base/files/file_util.h"
#include "base/files/file_util_proxy.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/ui/libgtk2ui/gtk2_util.h"
#include "chrome/browser/ui/libgtk2ui/printing_gtk2_util.h"
#include "printing/metafile.h"
#include "printing/print_job_constants.h"
#include "printing/print_settings.h"
#include "ui/aura/window.h"
#include "ui/views/widget/desktop_aura/x11_desktop_handler.h"

using content::BrowserThread;
using printing::PageRanges;
using printing::PrintSettings;

namespace {

// CUPS Duplex attribute and values.
const char kCUPSDuplex[] = "cups-Duplex";
const char kDuplexNone[] = "None";
const char kDuplexTumble[] = "DuplexTumble";
const char kDuplexNoTumble[] = "DuplexNoTumble";

int kPaperSizeTresholdMicrons = 100;
int kMicronsInMm = 1000;

// Checks whether gtk_paper_size can be used to represent user selected media.
// In fuzzy match mode checks that paper sizes are "close enough" (less than
// 1mm difference). In the exact mode, looks for the paper with the same PPD
// name and "close enough" size.
bool PaperSizeMatch(GtkPaperSize* gtk_paper_size,
                    const PrintSettings::RequestedMedia& media,
                    bool fuzzy_match) {
  if (!gtk_paper_size) {
    return false;
  }
  gfx::Size paper_size_microns(
      static_cast<int>(gtk_paper_size_get_width(gtk_paper_size, GTK_UNIT_MM) *
                       kMicronsInMm + 0.5),
      static_cast<int>(gtk_paper_size_get_height(gtk_paper_size, GTK_UNIT_MM) *
                       kMicronsInMm + 0.5));
  int diff = std::max(
      std::abs(paper_size_microns.width() - media.size_microns.width()),
      std::abs(paper_size_microns.height() - media.size_microns.height()));
  if (fuzzy_match) {
    return diff <= kPaperSizeTresholdMicrons;
  }
  return !media.vendor_id.empty() &&
         media.vendor_id == gtk_paper_size_get_ppd_name(gtk_paper_size) &&
         diff <= kPaperSizeTresholdMicrons;
}

// Looks up a paper size matching (in terms of PaperSizeMatch) the user selected
// media in the paper size list reported by GTK. Returns NULL if there's no
// match found.
GtkPaperSize* FindPaperSizeMatch(GList* gtk_paper_sizes,
                                 const PrintSettings::RequestedMedia& media) {
  GtkPaperSize* first_fuzzy_match = NULL;
  for (GList* p = gtk_paper_sizes; p && p->data; p = g_list_next(p)) {
    GtkPaperSize* gtk_paper_size = static_cast<GtkPaperSize*>(p->data);
    if (PaperSizeMatch(gtk_paper_size, media, false)) {
      return gtk_paper_size;
    }
    if (!first_fuzzy_match && PaperSizeMatch(gtk_paper_size, media, true)) {
      first_fuzzy_match = gtk_paper_size;
    }
  }
  return first_fuzzy_match;
}

class StickyPrintSettingGtk {
 public:
  StickyPrintSettingGtk() : last_used_settings_(gtk_print_settings_new()) {
  }
  ~StickyPrintSettingGtk() {
    NOTREACHED();  // Intended to be used with a Leaky LazyInstance.
  }

  GtkPrintSettings* settings() {
    return last_used_settings_;
  }

  void SetLastUsedSettings(GtkPrintSettings* settings) {
    DCHECK(last_used_settings_);
    g_object_unref(last_used_settings_);
    last_used_settings_ = gtk_print_settings_copy(settings);
  }

 private:
  GtkPrintSettings* last_used_settings_;

  DISALLOW_COPY_AND_ASSIGN(StickyPrintSettingGtk);
};

base::LazyInstance<StickyPrintSettingGtk>::Leaky g_last_used_settings =
    LAZY_INSTANCE_INITIALIZER;

// Helper class to track GTK printers.
class GtkPrinterList {
 public:
  GtkPrinterList() : default_printer_(NULL) {
    gtk_enumerate_printers(SetPrinter, this, NULL, TRUE);
  }

  ~GtkPrinterList() {
    for (std::vector<GtkPrinter*>::iterator it = printers_.begin();
         it < printers_.end(); ++it) {
      g_object_unref(*it);
    }
  }

  // Can return NULL if there's no default printer. E.g. Printer on a laptop
  // is "home_printer", but the laptop is at work.
  GtkPrinter* default_printer() {
    return default_printer_;
  }

  // Can return NULL if the printer cannot be found due to:
  // - Printer list out of sync with printer dialog UI.
  // - Querying for non-existant printers like 'Print to PDF'.
  GtkPrinter* GetPrinterWithName(const std::string& name) {
    if (name.empty())
      return NULL;

    for (std::vector<GtkPrinter*>::iterator it = printers_.begin();
         it < printers_.end(); ++it) {
      if (gtk_printer_get_name(*it) == name) {
        return *it;
      }
    }

    return NULL;
  }

 private:
  // Callback function used by gtk_enumerate_printers() to get all printer.
  static gboolean SetPrinter(GtkPrinter* printer, gpointer data) {
    GtkPrinterList* printer_list = reinterpret_cast<GtkPrinterList*>(data);
    if (gtk_printer_is_default(printer))
      printer_list->default_printer_ = printer;

    g_object_ref(printer);
    printer_list->printers_.push_back(printer);

    return FALSE;
  }

  std::vector<GtkPrinter*> printers_;
  GtkPrinter* default_printer_;
};

}  // namespace

// static
printing::PrintDialogGtkInterface* PrintDialogGtk2::CreatePrintDialog(
    PrintingContextLinux* context) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  return new PrintDialogGtk2(context);
}

PrintDialogGtk2::PrintDialogGtk2(PrintingContextLinux* context)
    : context_(context),
      dialog_(NULL),
      gtk_settings_(NULL),
      page_setup_(NULL),
      printer_(NULL) {
}

PrintDialogGtk2::~PrintDialogGtk2() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (dialog_) {
    aura::Window* parent = libgtk2ui::GetAuraTransientParent(dialog_);
    if (parent) {
      parent->RemoveObserver(this);
      libgtk2ui::ClearAuraTransientParent(dialog_);
    }
    gtk_widget_destroy(dialog_);
    dialog_ = NULL;
  }
  if (gtk_settings_) {
    g_object_unref(gtk_settings_);
    gtk_settings_ = NULL;
  }
  if (page_setup_) {
    g_object_unref(page_setup_);
    page_setup_ = NULL;
  }
  if (printer_) {
    g_object_unref(printer_);
    printer_ = NULL;
  }
}

void PrintDialogGtk2::UseDefaultSettings() {
  DCHECK(!page_setup_);
  DCHECK(!printer_);

  // |gtk_settings_| is a new copy.
  gtk_settings_ =
      gtk_print_settings_copy(g_last_used_settings.Get().settings());
  page_setup_ = gtk_page_setup_new();

  PrintSettings settings;
  InitPrintSettings(&settings);
}

bool PrintDialogGtk2::UpdateSettings(printing::PrintSettings* settings) {
  if (!gtk_settings_) {
    gtk_settings_ =
        gtk_print_settings_copy(g_last_used_settings.Get().settings());
  }

  scoped_ptr<GtkPrinterList> printer_list(new GtkPrinterList);
  printer_ = printer_list->GetPrinterWithName(
      base::UTF16ToUTF8(settings->device_name()));
  if (printer_) {
    g_object_ref(printer_);
    gtk_print_settings_set_printer(gtk_settings_,
                                   gtk_printer_get_name(printer_));
    if (!page_setup_) {
      page_setup_ = gtk_printer_get_default_page_size(printer_);
    }
  }

  gtk_print_settings_set_n_copies(gtk_settings_, settings->copies());
  gtk_print_settings_set_collate(gtk_settings_, settings->collate());

#if defined(USE_CUPS)
  std::string color_value;
  std::string color_setting_name;
  printing::GetColorModelForMode(settings->color(), &color_setting_name,
                                 &color_value);
  gtk_print_settings_set(gtk_settings_, color_setting_name.c_str(),
                         color_value.c_str());

  if (settings->duplex_mode() != printing::UNKNOWN_DUPLEX_MODE) {
    const char* cups_duplex_mode = NULL;
    switch (settings->duplex_mode()) {
      case printing::LONG_EDGE:
        cups_duplex_mode = kDuplexNoTumble;
        break;
      case printing::SHORT_EDGE:
        cups_duplex_mode = kDuplexTumble;
        break;
      case printing::SIMPLEX:
        cups_duplex_mode = kDuplexNone;
        break;
      default:  // UNKNOWN_DUPLEX_MODE
        NOTREACHED();
        break;
    }
    gtk_print_settings_set(gtk_settings_, kCUPSDuplex, cups_duplex_mode);
  }
#endif
  if (!page_setup_)
    page_setup_ = gtk_page_setup_new();

  if (page_setup_ && !settings->requested_media().IsDefault()) {
    const PrintSettings::RequestedMedia& requested_media =
        settings->requested_media();
    GtkPaperSize* gtk_current_paper_size =
        gtk_page_setup_get_paper_size(page_setup_);
    if (!PaperSizeMatch(gtk_current_paper_size, requested_media,
                        true /*fuzzy_match*/)) {
      GList* gtk_paper_sizes =
          gtk_paper_size_get_paper_sizes(false /*include_custom*/);
      if (gtk_paper_sizes) {
        GtkPaperSize* matching_gtk_paper_size =
            FindPaperSizeMatch(gtk_paper_sizes, requested_media);
        if (matching_gtk_paper_size) {
          VLOG(1) << "Using listed paper size";
          gtk_page_setup_set_paper_size(page_setup_, matching_gtk_paper_size);
        } else {
          VLOG(1) << "Using custom paper size";
          GtkPaperSize* custom_size = gtk_paper_size_new_custom(
              requested_media.vendor_id.c_str(),
              requested_media.vendor_id.c_str(),
              requested_media.size_microns.width() / kMicronsInMm,
              requested_media.size_microns.height() / kMicronsInMm,
              GTK_UNIT_MM);
          gtk_page_setup_set_paper_size(page_setup_, custom_size);
          gtk_paper_size_free(custom_size);
        }
#if GTK_CHECK_VERSION(2,28,0)
        g_list_free_full(gtk_paper_sizes,
                         reinterpret_cast<GDestroyNotify>(gtk_paper_size_free));
#else
        g_list_foreach(gtk_paper_sizes,
                       reinterpret_cast<GFunc>(gtk_paper_size_free), NULL);
        g_list_free(gtk_paper_sizes);
#endif
      }
    } else {
      VLOG(1) << "Using default paper size";
    }
  }

  gtk_print_settings_set_orientation(
      gtk_settings_,
      settings->landscape() ? GTK_PAGE_ORIENTATION_LANDSCAPE :
                              GTK_PAGE_ORIENTATION_PORTRAIT);

  InitPrintSettings(settings);
  return true;
}

void PrintDialogGtk2::ShowDialog(
    gfx::NativeView parent_view,
    bool has_selection,
    const PrintingContextLinux::PrintSettingsCallback& callback) {
  callback_ = callback;
  DCHECK(!callback_.is_null());

  dialog_ = gtk_print_unix_dialog_new(NULL, NULL);
  libgtk2ui::SetGtkTransientForAura(dialog_, parent_view);
  if (parent_view)
    parent_view->AddObserver(this);
  g_signal_connect(dialog_, "delete-event",
                   G_CALLBACK(gtk_widget_hide_on_delete), NULL);


  // Set modal so user cannot focus the same tab and press print again.
  gtk_window_set_modal(GTK_WINDOW(dialog_), TRUE);

  // Since we only generate PDF, only show printers that support PDF.
  // TODO(thestig) Add more capabilities to support?
  GtkPrintCapabilities cap = static_cast<GtkPrintCapabilities>(
      GTK_PRINT_CAPABILITY_GENERATE_PDF |
      GTK_PRINT_CAPABILITY_PAGE_SET |
      GTK_PRINT_CAPABILITY_COPIES |
      GTK_PRINT_CAPABILITY_COLLATE |
      GTK_PRINT_CAPABILITY_REVERSE);
  gtk_print_unix_dialog_set_manual_capabilities(GTK_PRINT_UNIX_DIALOG(dialog_),
                                                cap);
  gtk_print_unix_dialog_set_embed_page_setup(GTK_PRINT_UNIX_DIALOG(dialog_),
                                             TRUE);
  gtk_print_unix_dialog_set_support_selection(GTK_PRINT_UNIX_DIALOG(dialog_),
                                              TRUE);
  gtk_print_unix_dialog_set_has_selection(GTK_PRINT_UNIX_DIALOG(dialog_),
                                          has_selection);
  gtk_print_unix_dialog_set_settings(GTK_PRINT_UNIX_DIALOG(dialog_),
                                     gtk_settings_);
  g_signal_connect(dialog_, "response", G_CALLBACK(OnResponseThunk), this);
  gtk_widget_show(dialog_);

  // We need to call gtk_window_present after making the widgets visible to make
  // sure window gets correctly raised and gets focus.
  int time = views::X11DesktopHandler::get()->wm_user_time_ms();
  gtk_window_present_with_time(GTK_WINDOW(dialog_), time);
}

void PrintDialogGtk2::PrintDocument(const printing::MetafilePlayer& metafile,
                                    const base::string16& document_name) {
  // This runs on the print worker thread, does not block the UI thread.
  DCHECK(!BrowserThread::CurrentlyOn(BrowserThread::UI));

  // The document printing tasks can outlive the PrintingContext that created
  // this dialog.
  AddRef();

  bool success = base::CreateTemporaryFile(&path_to_pdf_);

  if (success) {
    base::File file;
    file.Initialize(path_to_pdf_,
                    base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
    success = metafile.SaveTo(&file);
    file.Close();
    if (!success)
      base::DeleteFile(path_to_pdf_, false);
  }

  if (!success) {
    LOG(ERROR) << "Saving metafile failed";
    // Matches AddRef() above.
    Release();
    return;
  }

  // No errors, continue printing.
  BrowserThread::PostTask(
      BrowserThread::UI,
      FROM_HERE,
      base::Bind(&PrintDialogGtk2::SendDocumentToPrinter, this, document_name));
}

void PrintDialogGtk2::AddRefToDialog() {
  AddRef();
}

void PrintDialogGtk2::ReleaseDialog() {
  Release();
}

void PrintDialogGtk2::OnResponse(GtkWidget* dialog, int response_id) {
  int num_matched_handlers = g_signal_handlers_disconnect_by_func(
      dialog_, reinterpret_cast<gpointer>(&OnResponseThunk), this);
  CHECK_EQ(1, num_matched_handlers);

  gtk_widget_hide(dialog_);

  switch (response_id) {
    case GTK_RESPONSE_OK: {
      if (gtk_settings_)
        g_object_unref(gtk_settings_);
      gtk_settings_ = gtk_print_unix_dialog_get_settings(
          GTK_PRINT_UNIX_DIALOG(dialog_));

      if (printer_)
        g_object_unref(printer_);
      printer_ = gtk_print_unix_dialog_get_selected_printer(
          GTK_PRINT_UNIX_DIALOG(dialog_));
      g_object_ref(printer_);

      if (page_setup_)
        g_object_unref(page_setup_);
      page_setup_ = gtk_print_unix_dialog_get_page_setup(
          GTK_PRINT_UNIX_DIALOG(dialog_));
      g_object_ref(page_setup_);

      // Handle page ranges.
      PageRanges ranges_vector;
      gint num_ranges;
      bool print_selection_only = false;
      switch (gtk_print_settings_get_print_pages(gtk_settings_)) {
        case GTK_PRINT_PAGES_RANGES: {
          GtkPageRange* gtk_range =
              gtk_print_settings_get_page_ranges(gtk_settings_, &num_ranges);
          if (gtk_range) {
            for (int i = 0; i < num_ranges; ++i) {
              printing::PageRange range;
              range.from = gtk_range[i].start;
              range.to = gtk_range[i].end;
              ranges_vector.push_back(range);
            }
            g_free(gtk_range);
          }
          break;
        }
        case GTK_PRINT_PAGES_SELECTION:
          print_selection_only = true;
          break;
        case GTK_PRINT_PAGES_ALL:
          // Leave |ranges_vector| empty to indicate print all pages.
          break;
        case GTK_PRINT_PAGES_CURRENT:
        default:
          NOTREACHED();
          break;
      }

      PrintSettings settings;
      settings.set_ranges(ranges_vector);
      settings.set_selection_only(print_selection_only);
      InitPrintSettingsGtk(gtk_settings_, page_setup_, &settings);
      context_->InitWithSettings(settings);
      callback_.Run(PrintingContextLinux::OK);
      callback_.Reset();
      return;
    }
    case GTK_RESPONSE_DELETE_EVENT:  // Fall through.
    case GTK_RESPONSE_CANCEL: {
      callback_.Run(PrintingContextLinux::CANCEL);
      callback_.Reset();
      return;
    }
    case GTK_RESPONSE_APPLY:
    default: {
      NOTREACHED();
    }
  }
}



static void OnJobCompletedThunk(GtkPrintJob* print_job,
                                gpointer user_data,
#if GTK_MAJOR_VERSION == 2
                                GError* error
#else
                                const GError* error
#endif
                                ) {
  static_cast<PrintDialogGtk2*>(user_data)->OnJobCompleted(print_job, error);
}
void PrintDialogGtk2::SendDocumentToPrinter(
    const base::string16& document_name) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // If |printer_| is NULL then somehow the GTK printer list changed out under
  // us. In which case, just bail out.
  if (!printer_) {
    // Matches AddRef() in PrintDocument();
    Release();
    return;
  }

  // Save the settings for next time.
  g_last_used_settings.Get().SetLastUsedSettings(gtk_settings_);

  GtkPrintJob* print_job = gtk_print_job_new(
      base::UTF16ToUTF8(document_name).c_str(),
      printer_,
      gtk_settings_,
      page_setup_);
  gtk_print_job_set_source_file(print_job, path_to_pdf_.value().c_str(), NULL);
  gtk_print_job_send(print_job, OnJobCompletedThunk, this, NULL);
}

void PrintDialogGtk2::OnJobCompleted(GtkPrintJob* print_job,
                                     const GError* error) {
  if (error)
    LOG(ERROR) << "Printing failed: " << error->message;
  if (print_job)
    g_object_unref(print_job);
  base::FileUtilProxy::DeleteFile(
      BrowserThread::GetMessageLoopProxyForThread(BrowserThread::FILE).get(),
      path_to_pdf_,
      false,
      base::FileUtilProxy::StatusCallback());
  // Printing finished. Matches AddRef() in PrintDocument();
  Release();
}

void PrintDialogGtk2::InitPrintSettings(PrintSettings* settings) {
  InitPrintSettingsGtk(gtk_settings_, page_setup_, settings);
  context_->InitWithSettings(*settings);
}

void PrintDialogGtk2::OnWindowDestroying(aura::Window* window) {
  DCHECK_EQ(libgtk2ui::GetAuraTransientParent(dialog_), window);

  libgtk2ui::ClearAuraTransientParent(dialog_);
  window->RemoveObserver(this);
  if (!callback_.is_null()) {
    callback_.Run(PrintingContextLinux::CANCEL);
    callback_.Reset();
  }
}
