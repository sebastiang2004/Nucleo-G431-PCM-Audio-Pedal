import 'package:flutter/material.dart';

import 'home/home_page.dart';

class DspComApp extends StatelessWidget {
  const DspComApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DSP COM',
      themeMode: ThemeMode.dark,
      darkTheme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFFE8E0D0),  // Cream to match artwork
        colorScheme: ColorScheme.dark(
          primary: const Color(0xFFC62828),
          secondary: const Color(0xFFD84315),
          surface: const Color(0xFFF5F0E6),
        ),
        appBarTheme: const AppBarTheme(
          backgroundColor: Color(0xFF2A2A2A),
          foregroundColor: Color(0xFFF5F0E6),
          elevation: 0,
        ),
      ),
      theme: ThemeData(useMaterial3: true),
      home: const HomePage(),
    );
  }
}
