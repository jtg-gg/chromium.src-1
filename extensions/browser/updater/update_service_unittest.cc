// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>

#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/values.h"
#include "components/crx_file/id_util.h"
#include "components/update_client/update_client.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_utils.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/browser/extensions_test.h"
#include "extensions/browser/mock_extension_system.h"
#include "extensions/browser/test_extensions_browser_client.h"
#include "extensions/browser/updater/update_service.h"
#include "extensions/common/extension_builder.h"
#include "extensions/common/value_builder.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

class FakeUpdateClient : public update_client::UpdateClient {
 public:
  FakeUpdateClient();

  // Returns the data we've gotten from the CrxDataCallback for ids passed to
  // the Update function.
  std::vector<update_client::CrxComponent>* data() { return &data_; }

  // update_client::UpdateClient
  void AddObserver(Observer* observer) override {}
  void RemoveObserver(Observer* observer) override {}
  void Install(const std::string& id,
               const CrxDataCallback& crx_data_callback,
               const CompletionCallback& completion_callback) override {}
  void Update(const std::vector<std::string>& ids,
              const CrxDataCallback& crx_data_callback,
              const CompletionCallback& completion_callback) override;
  bool GetCrxUpdateState(
      const std::string& id,
      update_client::CrxUpdateItem* update_item) const override {
    return false;
  }
  bool IsUpdating(const std::string& id) const override { return false; }
  void Stop() override {}
  void SendUninstallPing(const std::string& id,
                         const Version& version,
                         int reason) override {}

 protected:
  friend class base::RefCounted<FakeUpdateClient>;
  ~FakeUpdateClient() override {}

  std::vector<update_client::CrxComponent> data_;
};

FakeUpdateClient::FakeUpdateClient() {}

void FakeUpdateClient::Update(const std::vector<std::string>& ids,
                              const CrxDataCallback& crx_data_callback,
                              const CompletionCallback& completion_callback) {
  crx_data_callback.Run(ids, &data_);
}

}  // namespace

namespace extensions {

namespace {

// A fake ExtensionSystem that lets us intercept calls to install new
// versions of an extension.
class FakeExtensionSystem : public MockExtensionSystem {
 public:
  explicit FakeExtensionSystem(content::BrowserContext* context)
      : MockExtensionSystem(context) {}
  ~FakeExtensionSystem() override {}

  struct InstallUpdateRequest {
    std::string extension_id;
    base::FilePath temp_dir;
  };

  std::vector<InstallUpdateRequest>* install_requests() {
    return &install_requests_;
  }

  void set_install_callback(const base::Closure& callback) {
    next_install_callback_ = callback;
  }

  // ExtensionSystem override
  void InstallUpdate(const std::string& extension_id,
                     const base::FilePath& temp_dir) override {
    base::DeleteFile(temp_dir, true /*recursive*/);
    InstallUpdateRequest request;
    request.extension_id = extension_id;
    request.temp_dir = temp_dir;
    install_requests_.push_back(request);
    if (!next_install_callback_.is_null()) {
      base::Closure tmp = next_install_callback_;
      next_install_callback_.Reset();
      tmp.Run();
    }
  }

 private:
  std::vector<InstallUpdateRequest> install_requests_;
  base::Closure next_install_callback_;
};

class UpdateServiceTest : public ExtensionsTest {
 public:
  UpdateServiceTest() {
    extensions_browser_client()->set_extension_system_factory(
        &fake_extension_system_factory_);
  }
  ~UpdateServiceTest() override {}

  void SetUp() override {
    ExtensionsTest::SetUp();
    browser_threads_.reset(new content::TestBrowserThreadBundle(
        content::TestBrowserThreadBundle::DEFAULT));

    extensions_browser_client()->SetUpdateClientFactory(base::Bind(
        &UpdateServiceTest::CreateUpdateClient, base::Unretained(this)));

    update_service_ = UpdateService::Get(browser_context());
  }

 protected:
  UpdateService* update_service() const { return update_service_; }
  FakeUpdateClient* update_client() const { return update_client_.get(); }

  update_client::UpdateClient* CreateUpdateClient() {
    // We only expect that this will get called once, so consider it an error
    // if our update_client_ is already non-null.
    EXPECT_EQ(nullptr, update_client_.get());
    update_client_ = new FakeUpdateClient();
    return update_client_.get();
  }

  // Helper function that creates a file at |relative_path| within |directory|
  // and fills it with |content|.
  bool AddFileToDirectory(const base::FilePath& directory,
                          const base::FilePath& relative_path,
                          const std::string& content) {
    base::FilePath full_path = directory.Append(relative_path);
    if (!CreateDirectory(full_path.DirName()))
      return false;
    int result = base::WriteFile(full_path, content.data(), content.size());
    return (static_cast<size_t>(result) == content.size());
  }

  FakeExtensionSystem* extension_system() {
    return static_cast<FakeExtensionSystem*>(
        fake_extension_system_factory_.GetForBrowserContext(browser_context()));
  }

 private:
  UpdateService* update_service_;
  scoped_refptr<FakeUpdateClient> update_client_;
  scoped_ptr<content::TestBrowserThreadBundle> browser_threads_;
  MockExtensionSystemFactory<FakeExtensionSystem>
      fake_extension_system_factory_;
};

TEST_F(UpdateServiceTest, BasicUpdateOperations) {
  // Create a temporary directory that a fake extension will live in and fill
  // it with some test files.
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  base::FilePath foo_js(FILE_PATH_LITERAL("foo.js"));
  base::FilePath bar_html(FILE_PATH_LITERAL("bar/bar.html"));
  ASSERT_TRUE(AddFileToDirectory(temp_dir.path(), foo_js, "hello"))
      << "Failed to write " << temp_dir.path().value() << "/" << foo_js.value();
  ASSERT_TRUE(AddFileToDirectory(temp_dir.path(), bar_html, "world"));

  ExtensionBuilder builder;
  builder.SetManifest(std::move(DictionaryBuilder()
                                    .Set("name", "Foo")
                                    .Set("version", "1.0")
                                    .Set("manifest_version", 2)));
  builder.SetID(crx_file::id_util::GenerateId("whatever"));
  builder.SetPath(temp_dir.path());

  scoped_refptr<Extension> extension1(builder.Build());

  ExtensionRegistry::Get(browser_context())->AddEnabled(extension1);
  std::vector<std::string> ids;
  ids.push_back(extension1->id());

  // Start an update check and verify that the UpdateClient was sent the right
  // data.
  update_service()->StartUpdateCheck(ids);
  std::vector<update_client::CrxComponent>* data = update_client()->data();
  ASSERT_NE(nullptr, data);
  ASSERT_EQ(1u, data->size());

  ASSERT_EQ(data->at(0).version, *extension1->version());
  update_client::CrxInstaller* installer = data->at(0).installer.get();
  ASSERT_NE(installer, nullptr);

  // The GetInstalledFile method is used when processing differential updates
  // to get a path to an existing file in an extension. We want to test a
  // number of scenarios to be user we handle invalid relative paths, don't
  // accidentally return paths outside the extension's dir, etc.
  base::FilePath tmp;
  EXPECT_TRUE(installer->GetInstalledFile(foo_js.MaybeAsASCII(), &tmp));
  EXPECT_EQ(temp_dir.path().Append(foo_js), tmp) << tmp.value();

  EXPECT_TRUE(installer->GetInstalledFile(bar_html.MaybeAsASCII(), &tmp));
  EXPECT_EQ(temp_dir.path().Append(bar_html), tmp) << tmp.value();

  EXPECT_FALSE(installer->GetInstalledFile("does_not_exist", &tmp));
  EXPECT_FALSE(installer->GetInstalledFile("does/not/exist", &tmp));
  EXPECT_FALSE(installer->GetInstalledFile("/does/not/exist", &tmp));
  EXPECT_FALSE(installer->GetInstalledFile("C:\\tmp", &tmp));

  base::FilePath system_temp_dir;
  ASSERT_TRUE(base::GetTempDir(&system_temp_dir));
  EXPECT_FALSE(
      installer->GetInstalledFile(system_temp_dir.MaybeAsASCII(), &tmp));

  // Test the install callback.
  base::ScopedTempDir new_version_dir;
  ASSERT_TRUE(new_version_dir.CreateUniqueTempDir());
  scoped_ptr<base::DictionaryValue> new_manifest(
      extension1->manifest()->value()->DeepCopy());
  new_manifest->SetString("version", "2.0");

  installer->Install(*new_manifest, new_version_dir.path());

  scoped_refptr<content::MessageLoopRunner> loop_runner =
      new content::MessageLoopRunner();
  extension_system()->set_install_callback(loop_runner->QuitClosure());
  loop_runner->Run();

  std::vector<FakeExtensionSystem::InstallUpdateRequest>* requests =
      extension_system()->install_requests();
  ASSERT_EQ(1u, requests->size());
  EXPECT_EQ(requests->at(0).extension_id, extension1->id());
  EXPECT_NE(requests->at(0).temp_dir.value(), new_version_dir.path().value());
}

}  // namespace

}  // namespace extensions
