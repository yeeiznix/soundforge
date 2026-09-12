// SoundForge G0 — shell navigation graph.
package id.soundforge.pastudio.navigation

import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.NavHostController
import androidx.navigation.NavType
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.navArgument
import id.soundforge.pastudio.dsp.DspChainScreen
import id.soundforge.pastudio.home.HomeScreen
import id.soundforge.pastudio.measurement.MeasurementScreen
import id.soundforge.pastudio.mixer.MixerScreen
import id.soundforge.pastudio.prediction.PredictionScreen
import id.soundforge.pastudio.project.ProjectListScreen
import id.soundforge.pastudio.project.ProjectViewModel
import id.soundforge.pastudio.project.UiState
import id.soundforge.pastudio.reports.ReportsScreen
import id.soundforge.pastudio.scene.SceneEditorScreen
import id.soundforge.pastudio.signal.SignalEditorScreen
import id.soundforge.pastudio.training.TrainingScreen

/**
 * Root NavHost of the G0 shell. Start destination is the home dashboard.
 *
 * One shared [ProjectViewModel] is hoisted here (activity-scoped) and
 * forwarded to the home + project list screens so create/open state survives
 * navigation between them.
 */
@Composable
fun SoundForgeNavGraph(navController: NavHostController = rememberNavController()) {
    val projectViewModel: ProjectViewModel = viewModel()
    val uiState by projectViewModel.state.collectAsState()

    NavHost(navController = navController, startDestination = Route.Home.path) {
        composable(Route.Home.path) {
            HomeScreen(
                onCreateProject = projectViewModel::create,
                onOpenProjectList = { navController.navigate(Route.ProjectList.path) },
                openProjectName = (uiState as? UiState.Ready)?.meta?.name,
            )
        }

        composable(Route.ProjectList.path) {
            ProjectListScreen(
                viewModel = projectViewModel,
                onNavigateBack = { navController.popBackStack(Route.Home.path, inclusive = false) },
            )
        }

        // The eight editor placeholders accept an optional "projectId" argument.
        composable(
            route = Route.Scene.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            SceneEditorScreen(projectId = entry.arguments?.getString("projectId"))
        }

        composable(
            route = Route.Signal.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            SignalEditorScreen(projectId = entry.arguments?.getString("projectId"))
        }

        composable(
            route = Route.Mixer.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            MixerScreen(projectId = entry.arguments?.getString("projectId"))
        }

        composable(
            route = Route.Dsp.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            DspChainScreen(projectId = entry.arguments?.getString("projectId"))
        }

        composable(
            route = Route.Prediction.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            PredictionScreen(projectId = entry.arguments?.getString("projectId"))
        }

        composable(
            route = Route.Measurement.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            MeasurementScreen(projectId = entry.arguments?.getString("projectId"))
        }

        composable(
            route = Route.Training.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            TrainingScreen(projectId = entry.arguments?.getString("projectId"))
        }

        composable(
            route = Route.Reports.path + "?projectId={projectId}",
            arguments = listOf(projectIdArgument()),
        ) { entry ->
            ReportsScreen(projectId = entry.arguments?.getString("projectId"))
        }
    }
}

/** Optional nullable "projectId" nav argument shared by placeholder routes. */
private fun projectIdArgument() = navArgument("projectId") {
    type = NavType.StringType
    nullable = true
    defaultValue = null
}
