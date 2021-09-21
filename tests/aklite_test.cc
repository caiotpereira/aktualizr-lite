#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "test_utils.h"
#include "utilities/utils.h"

#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"
#include "target.h"
#include "uptane_generator/image_repo.h"

#include "fixtures/composeappenginetest.cc"
#include "fixtures/liteclienttest.cc"

class AkliteTest : public fixtures::ClientTest,
                   public fixtures::AppEngineTest,
                   public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();

    const auto app_engine_type{GetParam()};

    if (app_engine_type == "ComposeAppEngine") {
      app_engine =
          std::make_shared<Docker::ComposeAppEngine>(apps_root_dir, compose_cmd, docker_client_, registry_client_);
    } else if (app_engine_type == "RestorableAppEngine") {
      app_engine = std::make_shared<Docker::RestorableAppEngine>(
          fixtures::ClientTest::test_dir_.Path() / "apps-store", apps_root_dir, registry_client_, docker_client_,
          registry.getSkopeoClient(), daemon_.getUrl(), compose_cmd);
    } else {
      throw std::invalid_argument("Unsupported AppEngine type: " + app_engine_type);
    }
  }

  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none) override {
    const auto app_engine_type{GetParam()};

    if (app_engine_type == "ComposeAppEngine") {
      return ClientTest::createLiteClient(app_engine, initial_version, apps, apps_root_dir.string());
    } else if (app_engine_type == "RestorableAppEngine") {
      return ClientTest::createLiteClient(app_engine, initial_version, apps, apps_root_dir.string(), apps);
    } else {
      throw std::invalid_argument("Unsupported AppEngine type: " + app_engine_type);
    }
  }

 private:
};

TEST_P(AkliteTest, OstreeUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto new_target = createTarget();

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  checkEvents(*client, new_target, UpdateType::kOstree);
  ASSERT_FALSE(app_engine->isRunning(app01));
}

TEST_P(AkliteTest, AppUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto target01 = createAppTarget({app01});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));

  // update app
  auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
  auto target02 = createAppTarget({app01_updated});
  updateApps(*client, target01, target02);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target02));
  ASSERT_TRUE(app_engine->isRunning(app01_updated));
}

TEST_P(AkliteTest, AppRemoval) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto app02 = registry.addApp(fixtures::ComposeApp::create("app-02"));

  auto client =
      createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{"app-01", "app-02"}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));

  auto target01 = createAppTarget({app01, app02});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));
  ASSERT_TRUE(app_engine->isRunning(app02));

  reboot(client, boost::make_optional(std::vector<std::string>{"app-01"}));
  // make sure the "hadleRemovedApps" is called
  client->appsInSync();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  checkHeaders(*client, target01);
  checkEvents(*client, target01, UpdateType::kApp);
  ASSERT_TRUE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isFetched(app02));
  ASSERT_FALSE(app_engine->isRunning(app02));
}

TEST_P(AkliteTest, AppInvalidUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto target01 = createAppTarget({app01});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));

  // update app
  auto app01_updated =
      registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02", "incorrect-compose-file.yml"));
  auto target02 = createAppTarget({app01_updated});
  updateApps(*client, target01, target02, data::ResultCode::Numeric::kDownloadFailed);
  ASSERT_FALSE(targetsMatch(client->getCurrent(), target02));

  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));
}

TEST_P(AkliteTest, OstreeAndAppUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  std::vector<AppEngine::App> apps{app01};
  auto new_target = createTarget(&apps);

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  ASSERT_TRUE(app_engine->isRunning(app01));
  checkEvents(*client, new_target, UpdateType::kOstree);
}

TEST_P(AkliteTest, OstreeAndAppUpdateWithShortlist) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto app02 = registry.addApp(fixtures::ComposeApp::create("app-02"));

  auto client = createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{"app-02"}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // create new Target, both ostree and two Apps update
  std::vector<AppEngine::App> apps{app01, app02};
  auto new_target = createTarget(&apps);

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  checkEvents(*client, new_target, UpdateType::kOstree);
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_TRUE(app_engine->isRunning(app02));
}

TEST_P(AkliteTest, OstreeAndAppUpdateWithEmptyShortlist) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto app02 = registry.addApp(fixtures::ComposeApp::create("app-02"));

  auto client = createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{""}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // create new Target, both ostree and two Apps update
  std::vector<AppEngine::App> apps{app01, app02};
  auto new_target = createTarget(&apps);

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  checkEvents(*client, new_target, UpdateType::kOstree);
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));
}

TEST_P(AkliteTest, OstreeAndAppUpdateIfRollback) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update both rootfs and add new app
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  std::vector<AppEngine::App> apps{app01};
  auto target_01 = createTarget(&apps);

  {
    // update to the latest version
    update(*client, getInitialTarget(), target_01);
  }

  {
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);
    ASSERT_TRUE(app_engine->isRunning(app01));
  }

  {
    // update app, change image URL
    auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createTarget(&apps);
    // update to the latest version
    update(*client, target_01, target_02);
    // deploy the previous version/commit to emulate rollback
    getSysRepo().deploy(target_01.sha256Hash());

    reboot(client);
    // make sure that a rollback has happened and a client is still running the previous Target
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    // we stopped the original app before update
    ASSERT_FALSE(app_engine->isRunning(app01));
    ASSERT_FALSE(app_engine->isRunning(app01_updated));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);

    // emulate do_app_sync
    updateApps(*client, target_01, client->getCurrent());
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
  }
}

INSTANTIATE_TEST_SUITE_P(MultiEngine, AkliteTest, ::testing::Values("ComposeAppEngine", "RestorableAppEngine"));

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  // options passed as args in CMakeLists.txt
  fixtures::DeviceGatewayMock::RunCmd = argv[1];
  fixtures::SysRootFS::CreateCmd = argv[2];
  return RUN_ALL_TESTS();
}
