#define EXPORT_NAME TestDependency
#include <Geode/Loader.hpp>

API_INIT("com.geode.testdep")

class TestDependency {
public:
	static void depTest(GJGarageLayer* gl);
		// API_DECL(&TestDependency::depTest, gl);
};
